#include "network/websocket_client.h"
#include "config.h"
#include "log.h"
#include "utils/base64_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>

namespace Network {

WebSocketClient::WebSocketClient()
    : sockfd(-1), state(WebSocketState::DISCONNECTED), port(443), useTLS(true),
      sslContext(nullptr), sslConfig(nullptr), ctrDrbg(nullptr),
      entropy(nullptr), serverFd(nullptr) {}

WebSocketClient::~WebSocketClient() {
  disconnect();
  cleanupTLS();
}

bool WebSocketClient::parseUrl(const std::string &url) {
  size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos) {
    return false;
  }

  std::string protocol = url.substr(0, protocolEnd);
  useTLS = (protocol == "wss");
  port = useTLS ? 443 : 80;

  size_t hostStart = protocolEnd + 3;
  size_t slashPos = url.find('/', hostStart);
  size_t queryPos = url.find('?', hostStart);
  size_t pathStart = std::string::npos;

  if (slashPos != std::string::npos && queryPos != std::string::npos) {
    pathStart = std::min(slashPos, queryPos);
  } else if (slashPos != std::string::npos) {
    pathStart = slashPos;
  } else {
    pathStart = queryPos;
  }

  if (pathStart == std::string::npos) {
    path = "/";
    host = url.substr(hostStart);
  } else {
    host = url.substr(hostStart, pathStart - hostStart);
    path = url.substr(pathStart);
    if (path[0] == '?') {
      path = "/" + path;
    }
  }

  size_t portPos = host.find(':');
  if (portPos != std::string::npos) {
    port = std::stoi(host.substr(portPos + 1));
    host = host.substr(0, portPos);
  }

  Logger::log("[WS] Parsed URL: host=%s, port=%d, path=%s, tls=%d",
              host.c_str(), port, path.c_str(), useTLS);

  return true;
}

void WebSocketClient::cleanupTLS() {
  if (sslContext) {
    mbedtls_ssl_free((mbedtls_ssl_context *)sslContext);
    delete (mbedtls_ssl_context *)sslContext;
    sslContext = nullptr;
  }
  if (sslConfig) {
    mbedtls_ssl_config_free((mbedtls_ssl_config *)sslConfig);
    delete (mbedtls_ssl_config *)sslConfig;
    sslConfig = nullptr;
  }
  if (ctrDrbg) {
    mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context *)ctrDrbg);
    delete (mbedtls_ctr_drbg_context *)ctrDrbg;
    ctrDrbg = nullptr;
  }
  if (entropy) {
    mbedtls_entropy_free((mbedtls_entropy_context *)entropy);
    delete (mbedtls_entropy_context *)entropy;
    entropy = nullptr;
  }
  if (serverFd) {
    mbedtls_net_free((mbedtls_net_context *)serverFd);
    delete (mbedtls_net_context *)serverFd;
    serverFd = nullptr;
  }
}

int WebSocketClient::rawSend(const void *data, size_t len) {
  if (useTLS && sslContext) {
    return mbedtls_ssl_write((mbedtls_ssl_context *)sslContext,
                             (const unsigned char *)data, len);
  } else {
    return ::send(sockfd, data, len, 0);
  }
}

int WebSocketClient::rawRecv(void *data, size_t len) {
  if (useTLS && sslContext) {
    return mbedtls_ssl_read((mbedtls_ssl_context *)sslContext,
                            (unsigned char *)data, len);
  } else {
    return ::recv(sockfd, data, len, 0);
  }
}

std::string WebSocketClient::generateWebSocketKey() {
  uint8_t key[16];
  for (int i = 0; i < 16; i++) {
    key[i] = rand() % 256;
  }
  return Utils::Base64::encode(key, 16);
}

bool WebSocketClient::performHandshake() {
  std::string key = generateWebSocketKey();

  std::string request = "GET " + path +
                        " HTTP/1.1\r\n"
                        "Host: " +
                        host +
                        "\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Key: " +
                        key +
                        "\r\n"
                        "Sec-WebSocket-Version: 13\r\n"
                        "Origin: https://discord.com\r\n"
                        "User-Agent: " +
                        std::string(APP_USER_AGENT) +
                        "\r\n"
                        "\r\n";

  Logger::log("[WS] Sending handshake...");
  int sent = rawSend(request.c_str(), request.length());
  if (sent <= 0) {
    Logger::log("[WS] Failed to send handshake: %d", sent);
    return false;
  }

  char response[4096];
  int received = rawRecv(response, sizeof(response) - 1);
  if (received <= 0) {
    Logger::log("[WS] Failed to receive handshake response: %d", received);
    return false;
  }
  response[received] = '\0';

  if (strstr(response, "101") == nullptr) {
    Logger::log("[WS] Handshake failed: %s", response);
    return false;
  }

  Logger::log("[WS] Handshake successful (len: %d)", received);

  return true;
}

bool WebSocketClient::connect(const std::string &url) {
  if (state == WebSocketState::CONNECTED ||
      state == WebSocketState::CONNECTING) {
    Logger::log("WS connect called but already connected/connecting");
    return false;
  }

  Logger::log("WS connect %s", url.c_str());
  if (!parseUrl(url)) {
    Logger::log("WS parseUrl failed");
    return false;
  }

  state = WebSocketState::CONNECTING;

  serverFd = new mbedtls_net_context;
  sslContext = new mbedtls_ssl_context;
  sslConfig = new mbedtls_ssl_config;
  ctrDrbg = new mbedtls_ctr_drbg_context;
  entropy = new mbedtls_entropy_context;

  mbedtls_net_init((mbedtls_net_context *)serverFd);
  mbedtls_ssl_init((mbedtls_ssl_context *)sslContext);
  mbedtls_ssl_config_init((mbedtls_ssl_config *)sslConfig);
  mbedtls_ctr_drbg_init((mbedtls_ctr_drbg_context *)ctrDrbg);
  mbedtls_entropy_init((mbedtls_entropy_context *)entropy);

  int ret = mbedtls_ctr_drbg_seed(
      (mbedtls_ctr_drbg_context *)ctrDrbg, mbedtls_entropy_func,
      (mbedtls_entropy_context *)entropy, (const unsigned char *)"tricord", 10);

  if (ret != 0) {
    Logger::log("[WS] Failed to seed RNG: %d", ret);
    state = WebSocketState::DISCONNECTED;
    cleanupTLS();
    return false;
  }

  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);

  Logger::log("[WS] Connecting to %s:%s...", host.c_str(), portStr);
  ret = mbedtls_net_connect((mbedtls_net_context *)serverFd, host.c_str(),
                            portStr, MBEDTLS_NET_PROTO_TCP);

  if (ret != 0) {
    Logger::log("[WS] Failed to connect: %d", ret);
    state = WebSocketState::DISCONNECTED;
    cleanupTLS();
    return false;
  }

  ret = mbedtls_ssl_config_defaults(
      (mbedtls_ssl_config *)sslConfig, MBEDTLS_SSL_IS_CLIENT,
      MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);

  if (ret != 0) {
    Logger::log("[WS] Failed to set SSL defaults: %d", ret);
    state = WebSocketState::DISCONNECTED;
    cleanupTLS();
    return false;
  }

  mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)sslConfig,
                            MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng((mbedtls_ssl_config *)sslConfig, mbedtls_ctr_drbg_random,
                       ctrDrbg);

  ret = mbedtls_ssl_setup((mbedtls_ssl_context *)sslContext,
                          (mbedtls_ssl_config *)sslConfig);
  if (ret != 0) {
    Logger::log("[WS] Failed to setup SSL: %d", ret);
    state = WebSocketState::DISCONNECTED;
    cleanupTLS();
    return false;
  }

  ret =
      mbedtls_ssl_set_hostname((mbedtls_ssl_context *)sslContext, host.c_str());
  if (ret != 0) {
    Logger::log("[WS] Failed to set hostname: %d", ret);
    state = WebSocketState::DISCONNECTED;
    cleanupTLS();
    return false;
  }

  mbedtls_ssl_set_bio((mbedtls_ssl_context *)sslContext, serverFd,
                      mbedtls_net_send, mbedtls_net_recv, nullptr);

  Logger::log("[WS] Performing TLS handshake...");
  while ((ret = mbedtls_ssl_handshake((mbedtls_ssl_context *)sslContext)) !=
         0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      Logger::log("[WS] TLS handshake failed: %d", ret);
      state = WebSocketState::DISCONNECTED;
      cleanupTLS();
      return false;
    }
  }

  Logger::log("[WS] TLS handshake successful");

  if (!performHandshake()) {
    Logger::log("WS handshake failed");
    state = WebSocketState::DISCONNECTED;
    cleanupTLS();
    return false;
  }

  if (useTLS) {
    mbedtls_net_set_nonblock((mbedtls_net_context *)serverFd);
  }

  Logger::log("WS connected successfully");
  state = WebSocketState::CONNECTED;
  return true;
}

void WebSocketClient::disconnect(int code, const std::string &reason) {
  if (state == WebSocketState::DISCONNECTED ||
      state == WebSocketState::CLOSED) {
    return;
  }

  state = WebSocketState::CLOSING;

  {
    std::lock_guard<std::mutex> lock(sendMutex);
    std::vector<uint8_t> frame;
    uint8_t closeFrame[4];
    closeFrame[0] = (code >> 8) & 0xFF;
    closeFrame[1] = code & 0xFF;
    frame.push_back(0x80 | static_cast<uint8_t>(WebSocketOpcode::CLOSE));
    frame.push_back(0x80 | 2);

    uint8_t mask[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
      frame.push_back(mask[i]);
    }

    frame.push_back(closeFrame[0] ^ mask[0]);
    frame.push_back(closeFrame[1] ^ mask[1]);

    rawSend(frame.data(), frame.size());
  }

  cleanupTLS();

  state = WebSocketState::CLOSED;

  if (onClose) {
    onClose(code, reason);
  }
}

bool WebSocketClient::isConnected() const {
  return state == WebSocketState::CONNECTED;
}

WebSocketState WebSocketClient::getState() const { return state; }

bool WebSocketClient::sendFrame(WebSocketOpcode opcode, const void *data,
                                size_t len) {
  std::lock_guard<std::mutex> lock(sendMutex);
  std::vector<uint8_t> frame;

  frame.push_back(0x80 | static_cast<uint8_t>(opcode));

  if (len <= 125) {
    frame.push_back(0x80 | len);
  } else if (len <= 65535) {
    frame.push_back(0x80 | 126);
    frame.push_back((len >> 8) & 0xFF);
    frame.push_back(len & 0xFF);
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; i--) {
      frame.push_back((len >> (i * 8)) & 0xFF);
    }
  }

  uint8_t mask[4];
  for (int i = 0; i < 4; i++) {
    mask[i] = rand() % 256;
    frame.push_back(mask[i]);
  }

  const uint8_t *payload = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; i++) {
    frame.push_back(payload[i] ^ mask[i % 4]);
  }

  int sent = rawSend(frame.data(), frame.size());
  return sent == (int)frame.size();
}

bool WebSocketClient::send(const std::string &message) {
  if (state != WebSocketState::CONNECTED) {
    return false;
  }

  return sendFrame(WebSocketOpcode::TEXT, message.c_str(), message.length());
}

bool WebSocketClient::sendBinary(const std::vector<uint8_t> &data) {
  if (state != WebSocketState::CONNECTED) {
    return false;
  }

  return sendFrame(WebSocketOpcode::BINARY, data.data(), data.size());
}

bool WebSocketClient::recvExact(void *data, size_t len) {
  size_t totalReceived = 0;
  uint8_t *buf = static_cast<uint8_t *>(data);
  int retryCount = 0;

  while (totalReceived < len) {
    int r = rawRecv(buf + totalReceived, len - totalReceived);

    if (r > 0) {
      totalReceived += r;
      retryCount = 0;
    } else if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_TIMEOUT) {
      usleep(1000);
      retryCount++;
      if (retryCount > 5000) {
        if (onError)
          onError("recvExact timeout");
        return false;
      }
      continue;
    } else {

      if (r != 0) {
      }
      return false;
    }
  }
  return true;
}

bool WebSocketClient::receiveFrame(std::string &message) {
  uint8_t header[2];
  int received = rawRecv(header, 2);

  if (received <= 0) {
    if (received == MBEDTLS_ERR_SSL_WANT_READ ||
        received == MBEDTLS_ERR_SSL_TIMEOUT || received == -1) {
      return false;
    }
    if (received < -1) {
      if (onError) {
        char errMsg[64];
        snprintf(errMsg, sizeof(errMsg), "Recv error: %d", received);
        onError(errMsg);
      }
      disconnect();
    }
    return false;
  }

  if (received == 1) {
    if (!recvExact(header + 1, 1))
      return false;
  }

  bool fin = (header[0] & 0x80) != 0;
  (void)fin;
  WebSocketOpcode opcode = static_cast<WebSocketOpcode>(header[0] & 0x0F);
  bool masked = (header[1] & 0x80) != 0;
  size_t payloadLen = header[1] & 0x7F;

  if (payloadLen == 126) {
    uint8_t extLen[2];
    if (!recvExact(extLen, 2))
      return false;
    payloadLen = (extLen[0] << 8) | extLen[1];
  } else if (payloadLen == 127) {
    uint8_t extLen[8];
    if (!recvExact(extLen, 8))
      return false;
    payloadLen = 0;
    for (int i = 0; i < 8; i++) {
      payloadLen = (payloadLen << 8) | extLen[i];
    }
  }

  uint8_t mask[4] = {0};
  if (masked) {
    if (!recvExact(mask, 4))
      return false;
  }

  std::vector<uint8_t> payload(payloadLen);
  if (payloadLen > 0) {
    if (!recvExact(payload.data(), payloadLen)) {
      if (onError)
        onError("Failed to read payload");
      return false;
    }

    if (masked) {
      for (size_t i = 0; i < payloadLen; i++) {
        payload[i] ^= mask[i % 4];
      }
    }
  }

  switch (opcode) {
  case WebSocketOpcode::TEXT:
    message = std::string(payload.begin(), payload.end());
    return true;

  case WebSocketOpcode::BINARY:
    message = std::string(payload.begin(), payload.end());
    return true;

  case WebSocketOpcode::CLOSE:
    disconnect();
    return false;

  case WebSocketOpcode::PING:
    sendFrame(WebSocketOpcode::PONG, payload.data(), payload.size());
    return false;

  case WebSocketOpcode::PONG:
    return false;

  default:
    return false;
  }
}

void WebSocketClient::poll() {
  if (state != WebSocketState::CONNECTED) {
    return;
  }

  std::string message;
  if (receiveFrame(message) && !message.empty()) {
    if (onMessage) {
      onMessage(message);
    }
  }
}

void WebSocketClient::setOnMessage(MessageCallback callback) {
  onMessage = callback;
}

void WebSocketClient::setOnError(ErrorCallback callback) { onError = callback; }

void WebSocketClient::setOnClose(CloseCallback callback) { onClose = callback; }

} // namespace Network
