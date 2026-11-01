#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "network/http_client.h"
#include "utils/worker_thread.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Network {

enum class RequestPriority { REALTIME, INTERACTIVE, BACKGROUND };

// Non-const so a callback can take ownership of the body.
using ResponseCallback = std::function<void(HttpResponse &)>;

class NetworkManager {
  public:
	static NetworkManager &getInstance() {
		static NetworkManager instance;
		return instance;
	}

	void init(int interactiveCount = 1, int backgroundCount = 2);
	void shutdown();

	void enqueue(std::string url, std::string method, std::string body, RequestPriority priority,
	             ResponseCallback callback, std::map<std::string, std::string> extraHeaders = {});

	HttpResponse perform(HttpRequest request);

	void getQueueDepths(size_t &realtime, size_t &interactive, size_t &background);

  private:
	static constexpr int PRIORITY_COUNT = 3;

	struct SyncWait;
	struct Transfer;
	using TransferQueue = std::deque<std::unique_ptr<Transfer>>;

	NetworkManager();
	~NetworkManager();

	bool submit(std::unique_ptr<Transfer> transfer);
	std::unique_ptr<Transfer> takeNextLocked();
	bool hasQueuedLocked() const;

	void ioLoop();
	void startQueued();
	bool collectFinished();
	void finish(std::unique_ptr<Transfer> transfer, CURLcode result);
	void failAll();

	bool hasFinishedLocked(bool includeBackground) const;
	std::unique_ptr<Transfer> takeFinishedLocked(bool includeBackground);
	void callbackLoop(bool includeBackground);

	Utils::WorkerThread ioThread;
	Utils::WorkerThread interactiveCallbacks;
	Utils::WorkerThread backgroundCallbacks;

	int interactiveSlots = 1;
	int backgroundSlots = 2;
	int activeCount[PRIORITY_COUNT] = {};
	TransferQueue queued[PRIORITY_COUNT];
	TransferQueue finished[PRIORITY_COUNT];

	std::mutex mutex;
	std::condition_variable requestReady;
	std::condition_variable finishedReady;
	bool stop = false;

	CURLM *multi = nullptr;
	CURLSH *share = nullptr;
	std::vector<std::unique_ptr<Transfer>> active;
	std::vector<CURL *> idleHandles;
};

} // namespace Network

#endif // NETWORK_MANAGER_H
