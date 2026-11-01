#ifndef REMOTE_AUTH_H
#define REMOTE_AUTH_H

#include "network/websocket_client.h"
#include <3ds.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include "utils/worker_thread.h"

namespace Discord {

enum class RemoteAuthState { IDLE, CONNECTING, WAITING_FOR_SCAN, WAITING_FOR_CONFIRM, COMPLETED, FAILED, CANCELLED };

struct RemoteAuthUser {
	std::string id;
	std::string username;
	std::string discriminator;
	std::string avatar;
};

class RemoteAuth {
  public:
	static RemoteAuth &getInstance() {
		static RemoteAuth instance;
		return instance;
	}

	RemoteAuth();
	~RemoteAuth();

	bool start();

	void cancel();

	void poll();

	RemoteAuthState getState() const { return state; }

	std::string getQRCodeUrl() const;

	std::string decryptToken(const std::string &encryptedTokenBase64);

	void setOnStateChange(std::function<void(RemoteAuthState, const std::string &)> callback);
	void setOnUserScanned(std::function<void(const RemoteAuthUser &)> callback);
	void setOnTokenReceived(std::function<void(const std::string &)> callback);

  private:
	Network::WebSocketClient ws;
	std::atomic<RemoteAuthState> state;
	std::string fingerprint;
	uint64_t heartbeatInterval;
	uint64_t lastHeartbeat;
	uint64_t retryAt;
	const uint64_t retryDelay = 5000;

	void *rsaContext;
	std::string publicKeyBase64;
	std::string expectedFingerprint;

	std::atomic<bool> keyReady;
	std::atomic<bool> sessionWanted;
	std::atomic<bool> workerRunning;
	std::atomic<bool> retryImmediately;

	std::mutex eventMutex;
	std::deque<std::function<void()>> events;

	std::function<void(RemoteAuthState, const std::string &)> onStateChange;
	std::function<void(const RemoteAuthUser &)> onUserScanned;
	std::function<void(const std::string &)> onTokenReceived;

	Utils::WorkerThread workerThread;
	void launchWorker();
	void runSession();
	void serve();
	void post(std::function<void()> event);
	void handleMessage(std::string &message);
	void finish(const std::string &ticket);
	void sendHeartbeat();
	void setState(RemoteAuthState newState, const std::string &info = "");

	bool initRSA();
	void cleanupRSA();
	std::string exportPublicKeyBase64();
	std::string decrypt(const std::string &encryptedBase64);
	std::string decryptNonce(const std::string &encryptedNonceBase64);
};

} // namespace Discord

#endif // REMOTE_AUTH_H
