#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "network/http_client.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace Network {

enum class RequestPriority { REALTIME, INTERACTIVE, BACKGROUND };

struct AsyncRequest {
  std::string url;
  std::string method;
  std::string body;
  std::map<std::string, std::string> headers;
  RequestPriority priority;
  std::function<void(const HttpResponse &)> callback;

  bool operator<(const AsyncRequest &other) const {
    return priority > other.priority;
  }
};

class NetworkManager {
public:
  static NetworkManager &getInstance() {
    static NetworkManager instance;
    return instance;
  }

  void init(int interactiveCount = 1, int backgroundCount = 2);
  void shutdown();

  void enqueue(const std::string &url, const std::string &method,
               const std::string &body, RequestPriority priority,
               std::function<void(const HttpResponse &)> callback,
               const std::map<std::string, std::string> &extraHeaders = {});

  void get(const std::string &url, RequestPriority priority,
           std::function<void(const HttpResponse &)> callback);
  void post(const std::string &url, const std::string &body,
            RequestPriority priority,
            std::function<void(const HttpResponse &)> callback);

private:
  NetworkManager();
  ~NetworkManager();

  void workerThread(RequestPriority type);

  std::vector<std::thread> interactiveWorkers;
  std::vector<std::thread> backgroundWorkers;

  std::queue<AsyncRequest> realtimeQueue;
  std::queue<AsyncRequest> interactiveQueue;
  std::queue<AsyncRequest> backgroundQueue;

  std::mutex mutex;
  std::condition_variable condition;
  bool stop;

  CURLSH *curlShare;
  std::mutex dnsMutex;
  std::mutex sslMutex;

  static void lockCallback(CURL *handle, curl_lock_data data,
                           curl_lock_access access, void *userptr);
  static void unlockCallback(CURL *handle, curl_lock_data data, void *userptr);
};

} // namespace Network

#endif // NETWORK_MANAGER_H
