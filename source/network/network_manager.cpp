#include "network/network_manager.h"
#include "log.h"
#include "network/http_client.h"
#include "utils/message_utils.h"

namespace Network {

NetworkManager::NetworkManager() : stop(false), curlShare(nullptr) {
  curlShare = curl_share_init();
  curl_share_setopt(curlShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  curl_share_setopt(curlShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
  curl_share_setopt(curlShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);

  curl_share_setopt(curlShare, CURLSHOPT_LOCKFUNC, lockCallback);
  curl_share_setopt(curlShare, CURLSHOPT_UNLOCKFUNC, unlockCallback);
  curl_share_setopt(curlShare, CURLSHOPT_USERDATA, this);
}

NetworkManager::~NetworkManager() {
  shutdown();
  if (curlShare) {
    curl_share_cleanup(curlShare);
    curlShare = nullptr;
  }
}

void NetworkManager::lockCallback(CURL *handle, curl_lock_data data,
                                  curl_lock_access access, void *userptr) {
  NetworkManager *mgr = (NetworkManager *)userptr;
  if (data == CURL_LOCK_DATA_DNS) {
    mgr->dnsMutex.lock();
  } else if (data == CURL_LOCK_DATA_SSL_SESSION ||
             data == CURL_LOCK_DATA_CONNECT) {
    mgr->sslMutex.lock();
  }
}

void NetworkManager::unlockCallback(CURL *handle, curl_lock_data data,
                                    void *userptr) {
  NetworkManager *mgr = (NetworkManager *)userptr;
  if (data == CURL_LOCK_DATA_DNS) {
    mgr->dnsMutex.unlock();
  } else if (data == CURL_LOCK_DATA_SSL_SESSION ||
             data == CURL_LOCK_DATA_CONNECT) {
    mgr->sslMutex.unlock();
  }
}

void NetworkManager::init(int interactiveCount, int backgroundCount) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!interactiveWorkers.empty() || !backgroundWorkers.empty())
    return;

  stop = false;

  for (int i = 0; i < interactiveCount; ++i) {
    interactiveWorkers.emplace_back(&NetworkManager::workerThread, this,
                                    RequestPriority::INTERACTIVE);
  }

  for (int i = 0; i < backgroundCount; ++i) {
    backgroundWorkers.emplace_back(&NetworkManager::workerThread, this,
                                   RequestPriority::BACKGROUND);
  }

  Logger::log(
      "NetworkManager initialized: %d Interactive, %d Background threads",
      interactiveCount, backgroundCount);
}

void NetworkManager::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stop = true;
  }
  condition.notify_all();

  for (std::thread &worker : interactiveWorkers) {
    if (worker.joinable())
      worker.join();
  }
  for (std::thread &worker : backgroundWorkers) {
    if (worker.joinable())
      worker.join();
  }

  interactiveWorkers.clear();
  backgroundWorkers.clear();

  std::queue<AsyncRequest> empty0;
  std::swap(realtimeQueue, empty0);
  std::queue<AsyncRequest> empty1;
  std::swap(interactiveQueue, empty1);
  std::queue<AsyncRequest> empty2;
  std::swap(backgroundQueue, empty2);

  Logger::log("NetworkManager shutdown");
}

void NetworkManager::enqueue(
    const std::string &url, const std::string &method, const std::string &body,
    RequestPriority priority,
    std::function<void(const HttpResponse &)> callback,
    const std::map<std::string, std::string> &extraHeaders) {

  {
    std::lock_guard<std::mutex> lock(mutex);
    AsyncRequest req;
    req.url = url;
    req.method = method;
    req.body = body;
    req.priority = priority;
    req.callback = callback;
    req.headers = extraHeaders;

    if (priority == RequestPriority::REALTIME) {
      realtimeQueue.push(req);
    } else if (priority == RequestPriority::INTERACTIVE) {
      interactiveQueue.push(req);
    } else {
      backgroundQueue.push(req);
    }
  }
  condition.notify_all();
}

void NetworkManager::workerThread(RequestPriority type) {
  HttpClient client;
  client.setVerifySSL(true);
  client.setShareHandle(curlShare);

  while (true) {
    AsyncRequest req;
    bool hasRequest = false;

    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [this, type] {
        if (stop)
          return true;

        bool hasHighPriority =
            !realtimeQueue.empty() || !interactiveQueue.empty();
        bool hasBackground = !backgroundQueue.empty();

        if (type == RequestPriority::INTERACTIVE) {
          return hasHighPriority;
        } else {
          return hasHighPriority || hasBackground;
        }
      });

      if (stop)
        return;

      if (!realtimeQueue.empty()) {
        req = std::move(realtimeQueue.front());
        realtimeQueue.pop();
        hasRequest = true;
      } else if (!interactiveQueue.empty()) {
        req = std::move(interactiveQueue.front());
        interactiveQueue.pop();
        hasRequest = true;
      } else if (!backgroundQueue.empty() &&
                 type != RequestPriority::INTERACTIVE) {
        req = std::move(backgroundQueue.front());
        backgroundQueue.pop();
        hasRequest = true;
      }
    }

    if (hasRequest) {
      HttpResponse resp;
      if (req.method == "GET") {
        resp = client.get(req.url, req.headers);
      } else if (req.method == "POST") {
        resp = client.post(req.url, req.body, req.headers);
      } else if (req.method == "PATCH") {
        resp = client.patch(req.url, req.body, req.headers);
      } else if (req.method == "DELETE") {
        resp = client.del(req.url, req.headers);
      }

      if (req.callback) {
        req.callback(resp);
      }

      auto it = resp.headers.find("Date");
      if (it != resp.headers.end()) {
        UI::MessageUtils::syncClock(it->second);
      }
    }
  }
}

} // namespace Network
