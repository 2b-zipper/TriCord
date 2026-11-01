#include "network/network_manager.h"
#include "config.h"
#include "log.h"
#include "utils/message_utils.h"
#include "utils/redraw.h"

#include <3ds.h>
#include <algorithm>

namespace Network {

namespace {
constexpr int REALTIME = (int)RequestPriority::REALTIME;
constexpr int INTERACTIVE = (int)RequestPriority::INTERACTIVE;
constexpr int BACKGROUND = (int)RequestPriority::BACKGROUND;
constexpr int64_t MIN_TICK_NS = 1000000LL;
constexpr int64_t MAX_TICK_NS = 10000000LL;
constexpr long EXTRA_CACHED_CONNECTIONS = 2;
} // namespace

struct NetworkManager::SyncWait {
	LightEvent done;
	HttpResponse response;
};

struct NetworkManager::Transfer {
	HttpRequest request;
	int priority = INTERACTIVE;
	ResponseCallback callback;
	SyncWait *waiter = nullptr;
	HttpResponse response;
	CURL *easy = nullptr;
	curl_slist *headers = nullptr;
};

NetworkManager::NetworkManager() = default;

NetworkManager::~NetworkManager() { shutdown(); }

void NetworkManager::init(int interactiveCount, int backgroundCount) {
	std::lock_guard<std::mutex> lock(mutex);
	if (ioThread.joinable()) {
		return;
	}

	stop = false;
	interactiveSlots = interactiveCount;
	backgroundSlots = backgroundCount;
	initHttpDefaults();

	ioThread.start([this] { ioLoop(); }, 1, 32 * 1024, true);
	interactiveCallbacks.start([this] { callbackLoop(false); }, 2);
	backgroundCallbacks.start([this] { callbackLoop(true); }, 3);

	Logger::log("NetworkManager initialized: %d transfers, %d of them background",
	            1 + interactiveCount + backgroundCount, backgroundCount);
}

void NetworkManager::shutdown() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		stop = true;
	}
	requestReady.notify_all();
	finishedReady.notify_all();

	if (ioThread.joinable()) {
		ioThread.join();
	}
	if (interactiveCallbacks.joinable()) {
		interactiveCallbacks.join();
	}
	if (backgroundCallbacks.joinable()) {
		backgroundCallbacks.join();
	}

	for (int i = 0; i < PRIORITY_COUNT; i++) {
		finished[i].clear();
	}

	Logger::log("NetworkManager shutdown");
}

bool NetworkManager::submit(std::unique_ptr<Transfer> transfer) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stop || !ioThread.joinable()) {
			return false;
		}
		queued[transfer->priority].push_back(std::move(transfer));
	}
	requestReady.notify_one();
	return true;
}

void NetworkManager::enqueue(std::string url, std::string method, std::string body, RequestPriority priority,
                             ResponseCallback callback, std::map<std::string, std::string> extraHeaders) {
	auto transfer = std::make_unique<Transfer>();
	transfer->request = {std::move(url), std::move(method), std::move(body), std::move(extraHeaders),
	                     HTTP_TIMEOUT_SECONDS};
	transfer->priority = (int)priority;
	transfer->callback = std::move(callback);
	submit(std::move(transfer));
}

HttpResponse NetworkManager::perform(HttpRequest request) {
	SyncWait wait;
	LightEvent_Init(&wait.done, RESET_ONESHOT);

	auto transfer = std::make_unique<Transfer>();
	transfer->request = std::move(request);
	transfer->priority = REALTIME;
	transfer->waiter = &wait;
	if (!submit(std::move(transfer))) {
		wait.response.error = "Network is not running";
		return std::move(wait.response);
	}

	LightEvent_Wait(&wait.done);
	return std::move(wait.response);
}

void NetworkManager::getQueueDepths(size_t &realtime, size_t &interactive, size_t &background) {
	std::lock_guard<std::mutex> lock(mutex);
	realtime = queued[REALTIME].size();
	interactive = queued[INTERACTIVE].size();
	background = queued[BACKGROUND].size();
}

bool NetworkManager::hasQueuedLocked() const {
	return !queued[REALTIME].empty() || !queued[INTERACTIVE].empty() || !queued[BACKGROUND].empty();
}

std::unique_ptr<NetworkManager::Transfer> NetworkManager::takeNextLocked() {
	const int sharedSlots = interactiveSlots + backgroundSlots;
	const int running = activeCount[REALTIME] + activeCount[INTERACTIVE] + activeCount[BACKGROUND];

	int priority = -1;
	if (running <= sharedSlots && !queued[REALTIME].empty()) {
		priority = REALTIME;
	} else if (running < sharedSlots && !queued[INTERACTIVE].empty()) {
		priority = INTERACTIVE;
	} else if (running < sharedSlots && activeCount[BACKGROUND] < backgroundSlots && !queued[BACKGROUND].empty()) {
		priority = BACKGROUND;
	}
	if (priority < 0) {
		return nullptr;
	}

	std::unique_ptr<Transfer> next = std::move(queued[priority].front());
	queued[priority].pop_front();
	activeCount[priority]++;
	return next;
}

void NetworkManager::startQueued() {
	while (true) {
		std::unique_ptr<Transfer> transfer;
		{
			std::lock_guard<std::mutex> lock(mutex);
			transfer = takeNextLocked();
		}
		if (!transfer) {
			return;
		}

		CURL *easy = nullptr;
		if (!idleHandles.empty()) {
			easy = idleHandles.back();
			idleHandles.pop_back();
		} else {
			easy = curl_easy_init();
			if (easy) {
				curl_easy_setopt(easy, CURLOPT_SHARE, share);
			}
		}
		if (!easy) {
			finish(std::move(transfer), CURLE_FAILED_INIT);
			continue;
		}

		transfer->easy = easy;
		transfer->headers = prepareTransfer(easy, transfer->request, transfer->response);
		curl_multi_add_handle(multi, easy);
		active.push_back(std::move(transfer));
	}
}

void NetworkManager::finish(std::unique_ptr<Transfer> transfer, CURLcode result) {
	HttpResponse &response = transfer->response;
	if (result == CURLE_OK) {
		curl_easy_getinfo(transfer->easy, CURLINFO_RESPONSE_CODE, &response.statusCode);
		response.success = response.statusCode >= 200 && response.statusCode < 300;
	} else {
		response.error = curl_easy_strerror(result);
	}

	if (transfer->easy) {
		curl_multi_remove_handle(multi, transfer->easy);
		curl_slist_free_all(transfer->headers);
		idleHandles.push_back(transfer->easy);
		transfer->easy = nullptr;
	}

	const int priority = transfer->priority;
	if (transfer->waiter) {
		transfer->waiter->response = std::move(response);
		LightEvent_Signal(&transfer->waiter->done);
		std::lock_guard<std::mutex> lock(mutex);
		activeCount[priority]--;
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		activeCount[priority]--;
		finished[priority].push_back(std::move(transfer));
	}
	finishedReady.notify_all();
}

bool NetworkManager::collectFinished() {
	bool any = false;
	int remaining = 0;
	while (CURLMsg *message = curl_multi_info_read(multi, &remaining)) {
		if (message->msg != CURLMSG_DONE) {
			continue;
		}
		CURL *easy = message->easy_handle;
		const CURLcode result = message->data.result;
		auto it = std::find_if(active.begin(), active.end(),
		                       [easy](const std::unique_ptr<Transfer> &t) { return t->easy == easy; });
		if (it == active.end()) {
			continue;
		}
		std::unique_ptr<Transfer> transfer = std::move(*it);
		active.erase(it);
		finish(std::move(transfer), result);
		any = true;
	}
	return any;
}

void NetworkManager::failAll() {
	for (std::unique_ptr<Transfer> &transfer : active) {
		curl_multi_remove_handle(multi, transfer->easy);
		curl_slist_free_all(transfer->headers);
		curl_easy_cleanup(transfer->easy);
		if (transfer->waiter) {
			transfer->waiter->response.error = "Network is shutting down";
			LightEvent_Signal(&transfer->waiter->done);
		}
	}
	active.clear();

	std::lock_guard<std::mutex> lock(mutex);
	for (int i = 0; i < PRIORITY_COUNT; i++) {
		for (std::unique_ptr<Transfer> &transfer : queued[i]) {
			if (transfer->waiter) {
				transfer->waiter->response.error = "Network is shutting down";
				LightEvent_Signal(&transfer->waiter->done);
			}
		}
		queued[i].clear();
		activeCount[i] = 0;
	}
}

void NetworkManager::ioLoop() {
	multi = curl_multi_init();
	share = curl_share_init();
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
	curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, 1L + interactiveSlots + backgroundSlots + EXTRA_CACHED_CONNECTIONS);

	while (true) {
		{
			std::unique_lock<std::mutex> lock(mutex);
			requestReady.wait(lock, [this] { return stop || !active.empty() || hasQueuedLocked(); });
			if (stop) {
				break;
			}
		}

		startQueued();

		int running = 0;
		curl_multi_perform(multi, &running);
		if (collectFinished()) {
			continue;
		}

		// Not curl_multi_wait: it would sit in poll, and every thread shares one socket service session.
		long timeoutMs = -1;
		curl_multi_timeout(multi, &timeoutMs);
		int64_t wait = MAX_TICK_NS;
		if (timeoutMs >= 0) {
			wait = std::clamp<int64_t>(timeoutMs * 1000000LL, MIN_TICK_NS, MAX_TICK_NS);
		}
		svcSleepThread(wait);
	}

	failAll();
	for (CURL *easy : idleHandles) {
		curl_easy_cleanup(easy);
	}
	idleHandles.clear();
	curl_multi_cleanup(multi);
	multi = nullptr;
	curl_share_cleanup(share);
	share = nullptr;
}

bool NetworkManager::hasFinishedLocked(bool includeBackground) const {
	return !finished[REALTIME].empty() || !finished[INTERACTIVE].empty() ||
	       (includeBackground && !finished[BACKGROUND].empty());
}

std::unique_ptr<NetworkManager::Transfer> NetworkManager::takeFinishedLocked(bool includeBackground) {
	for (int priority = REALTIME; priority <= (includeBackground ? BACKGROUND : INTERACTIVE); priority++) {
		if (!finished[priority].empty()) {
			std::unique_ptr<Transfer> transfer = std::move(finished[priority].front());
			finished[priority].pop_front();
			return transfer;
		}
	}
	return nullptr;
}

void NetworkManager::callbackLoop(bool includeBackground) {
	while (true) {
		std::unique_ptr<Transfer> transfer;
		{
			std::unique_lock<std::mutex> lock(mutex);
			finishedReady.wait(lock,
			                   [this, includeBackground] { return stop || hasFinishedLocked(includeBackground); });
			if (stop) {
				return;
			}
			transfer = takeFinishedLocked(includeBackground);
		}

		if (transfer->callback) {
			transfer->callback(transfer->response);
		}
		Utils::requestRedraw();

		if (!transfer->response.date.empty()) {
			UI::MessageUtils::syncClock(transfer->response.date);
		}
	}
}

} // namespace Network
