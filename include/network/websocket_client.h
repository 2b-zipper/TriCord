#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Network {

enum class WebSocketState { DISCONNECTED, CONNECTING, CONNECTED, CLOSING, CLOSED };

enum class WebSocketOpcode : uint8_t {
	CONTINUATION = 0x0,
	TEXT = 0x1,
	BINARY = 0x2,
	CLOSE = 0x8,
	PING = 0x9,
	PONG = 0xA
};

class WebSocketClient {
  public:
	using MessageCallback = std::function<void(std::string &)>;
	using ErrorCallback = std::function<void(const std::string &)>;
	using CloseCallback = std::function<void(int code, const std::string &reason)>;

	static constexpr int64_t BUSY_POLL_NS = 1000000LL;
	static constexpr int64_t IDLE_POLL_NS = 20000000LL;

	WebSocketClient();
	~WebSocketClient();

	WebSocketClient(const WebSocketClient &) = delete;
	WebSocketClient &operator=(const WebSocketClient &) = delete;

	bool connect(const std::string &url);
	void disconnect(int code = 1000, const std::string &reason = "");
	bool isConnected() const;
	WebSocketState getState() const;

	bool send(const std::string &message);
	bool sendBinary(const std::vector<uint8_t> &data);

	void setOnMessage(MessageCallback callback);
	void setOnBinaryMessage(MessageCallback callback);
	void setOnError(ErrorCallback callback);
	void setOnClose(CloseCallback callback);

	bool poll();

  private:
	struct Tls;
	enum class ReadStep { IDLE, PROGRESS, FRAME, FAILED };

	static constexpr size_t MAX_RX_HEADER = 10;

	bool parseUrl(const std::string &url);
	bool openSocket(Tls &session, uint64_t deadline);
	bool startTls(Tls &session, uint64_t deadline);
	bool upgrade(Tls &session, uint64_t deadline);

	bool sendFrame(WebSocketOpcode opcode, const void *data, size_t len);
	bool writeFrame(WebSocketOpcode opcode, const void *data, size_t len, uint64_t timeoutMs);

	size_t rxHeaderNeeded() const;
	bool beginPayload(std::string &error);
	ReadStep readEnded(int result, std::string &error) const;
	ReadStep readSome(std::string &error);
	void resetRx();
	void dispatch(WebSocketOpcode opcode, std::string &payload);

	bool beginClose();
	void finishClose(int code, const std::string &reason);
	void fail(const std::string &error);

	std::atomic<WebSocketState> state{WebSocketState::DISCONNECTED};
	std::string host;
	int port = 443;
	std::string path;

	MessageCallback onMessage;
	MessageCallback onBinaryMessage;
	ErrorCallback onError;
	CloseCallback onClose;

	std::unique_ptr<Tls> tls;
	std::mutex ioMutex;
	bool writeBroken = false;

	uint8_t rxHeader[MAX_RX_HEADER];
	size_t rxHeaderSize = 0;
	bool rxInPayload = false;
	std::string rxPayload;
	size_t rxPayloadSize = 0;
};

} // namespace Network

#endif // WEBSOCKET_CLIENT_H
