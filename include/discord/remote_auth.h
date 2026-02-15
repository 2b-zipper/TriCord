#ifndef REMOTE_AUTH_H
#define REMOTE_AUTH_H

#include "network/websocket_client.h"
#include <3ds.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Discord {

enum class RemoteAuthState {
  IDLE,
  CONNECTING,
  WAITING_FOR_SCAN,
  WAITING_FOR_CONFIRM,
  COMPLETED,
  FAILED,
  CANCELLED
};

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

  void prepare();

  bool start();

  void cancel();

  void poll();

  RemoteAuthState getState() const { return state; }

  std::string getQRCodeUrl() const;

  std::string decryptToken(const std::string &encryptedTokenBase64);

  // Callbacks
  void setOnStateChange(
      std::function<void(RemoteAuthState, const std::string &)> callback);
  void setOnUserScanned(std::function<void(const RemoteAuthUser &)> callback);
  void setOnTokenReceived(std::function<void(const std::string &)> callback);

private:
  Network::WebSocketClient ws;
  RemoteAuthState state;
  std::string fingerprint;
  std::string ticket;
  uint64_t heartbeatInterval;
  uint64_t lastHeartbeat;
  uint64_t lastRetryTime;
  const uint64_t retryDelay = 5000;

  void *rsaContext;
  void *ctrDrbgContext;
  void *entropyContext;
  std::string publicKeyBase64;

  bool isInitializing;
  bool initSuccess;
  void runInit();

  // Callbacks
  std::function<void(RemoteAuthState, const std::string &)> onStateChange;
  std::function<void(const RemoteAuthUser &)> onUserScanned;
  std::function<void(const std::string &)> onTokenReceived;

  void handleMessage(std::string &message);
  void sendHeartbeat();
  void setState(RemoteAuthState newState, const std::string &info = "");

  bool initRSA();
  void cleanupRSA();
  std::string exportPublicKeyBase64();
  std::string decryptNonce(const std::string &encryptedNonceBase64);
};

} // namespace Discord

#endif // REMOTE_AUTH_H
