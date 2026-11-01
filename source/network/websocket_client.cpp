#include "network/websocket_client.h"
#include "config.h"
#include "log.h"
#include "network/tls.h"
#include "utils/base64_utils.h"
#include "utils/system_utils.h"

#include <3ds.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

namespace Network {

namespace {
constexpr size_t LARGE_FRAME = 64 * 1024;
constexpr size_t FRAME_HEAP_MARGIN = 1024 * 1024;
constexpr size_t MAX_TX_HEADER = 14;
constexpr size_t MAX_UPGRADE_RESPONSE = 8 * 1024;
constexpr uint64_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint64_t WRITE_TIMEOUT_MS = 5000;
constexpr uint64_t CLOSE_TIMEOUT_MS = 500;
constexpr int64_t CONNECT_POLL_NS = 5000000LL;
constexpr int64_t RETRY_NS = 2000000LL;
constexpr int ABNORMAL_CLOSE_CODE = 1006;

bool socketWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

int socketSend(void *fd, const unsigned char *data, size_t len) {
	const int sent = ::send(*static_cast<int *>(fd), data, len, 0);
	if (sent >= 0) {
		return sent;
	}
	return socketWouldBlock() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

int socketRecv(void *fd, unsigned char *data, size_t len) {
	const int received = ::recv(*static_cast<int *>(fd), data, len, 0);
	if (received >= 0) {
		return received;
	}
	return socketWouldBlock() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

bool tlsWouldBlock(int result) { return result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE; }

bool connectBefore(int fd, const addrinfo *address, uint64_t deadline) {
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
		return true;
	}
	if (errno != EINPROGRESS && !socketWouldBlock()) {
		return false;
	}

	pollfd entry{};
	entry.fd = fd;
	entry.events = POLLOUT;
	while (osGetTime() < deadline) {
		const int ready = ::poll(&entry, 1, 0);
		if (ready != 0) {
			return ready > 0 && (entry.revents & POLLOUT) && !(entry.revents & (POLLERR | POLLHUP));
		}
		svcSleepThread(CONNECT_POLL_NS);
	}
	return false;
}

bool writeAll(mbedtls_ssl_context &ssl, const uint8_t *data, size_t len, uint64_t deadline) {
	size_t written = 0;
	while (written < len) {
		const int result = mbedtls_ssl_write(&ssl, data + written, len - written);
		if (result > 0) {
			written += result;
		} else if (!tlsWouldBlock(result) || osGetTime() >= deadline) {
			return false;
		} else {
			svcSleepThread(RETRY_NS);
		}
	}
	return true;
}

std::string generateWebSocketKey() {
	uint8_t key[16];
	for (uint8_t &byte : key) {
		byte = rand() % 256;
	}
	return Utils::Base64::encode(key, sizeof(key));
}
} // namespace

struct WebSocketClient::Tls {
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config config;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_entropy_context entropy;
	int fd = -1;

	Tls() {
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&config);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_entropy_init(&entropy);
	}

	~Tls() {
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&config);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		if (fd >= 0) {
			::close(fd);
		}
	}
};

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient() { disconnect(); }

bool WebSocketClient::parseUrl(const std::string &url) {
	constexpr const char *SCHEME = "wss://";
	constexpr size_t SCHEME_LENGTH = 6;
	if (url.compare(0, SCHEME_LENGTH, SCHEME) != 0) {
		return false;
	}

	const size_t pathStart = url.find_first_of("/?", SCHEME_LENGTH);
	if (pathStart == std::string::npos) {
		host = url.substr(SCHEME_LENGTH);
		path = "/";
	} else {
		host = url.substr(SCHEME_LENGTH, pathStart - SCHEME_LENGTH);
		path = url.substr(pathStart);
		if (path[0] == '?') {
			path = "/" + path;
		}
	}

	port = 443;
	const size_t portPos = host.find(':');
	if (portPos != std::string::npos) {
		port = std::stoi(host.substr(portPos + 1));
		host = host.substr(0, portPos);
	}

	return true;
}

bool WebSocketClient::openSocket(Tls &session, uint64_t deadline) {
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo *addresses = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses) != 0) {
		Logger::log("[WS] Failed to resolve %s", host.c_str());
		return false;
	}

	for (const addrinfo *address = addresses; address && session.fd < 0; address = address->ai_next) {
		const int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connectBefore(fd, address, deadline)) {
			session.fd = fd;
		} else {
			::close(fd);
		}
	}
	freeaddrinfo(addresses);

	if (session.fd < 0) {
		Logger::log("[WS] Failed to connect to %s:%d", host.c_str(), port);
	}
	return session.fd >= 0;
}

bool WebSocketClient::startTls(Tls &session, uint64_t deadline) {
	static const unsigned char SEED[] = "tricord";
	int result = mbedtls_ctr_drbg_seed(&session.drbg, mbedtls_entropy_func, &session.entropy, SEED, sizeof(SEED) - 1);
	if (result == 0) {
		result = mbedtls_ssl_config_defaults(&session.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
		                                     MBEDTLS_SSL_PRESET_DEFAULT);
	}
	if (result != 0) {
		Logger::log("[WS] TLS setup failed: -0x%04x", -result);
		return false;
	}

	const bool verify = !Config::getInstance().isSslVerificationDisabled();
	mbedtls_ssl_conf_authmode(&session.config, verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
	if (verify) {
		mbedtls_ssl_conf_ca_chain(&session.config, sharedCaChain(), nullptr);
	}
	mbedtls_ssl_conf_rng(&session.config, mbedtls_ctr_drbg_random, &session.drbg);

	result = mbedtls_ssl_setup(&session.ssl, &session.config);
	if (result == 0) {
		result = mbedtls_ssl_set_hostname(&session.ssl, host.c_str());
	}
	if (result != 0) {
		Logger::log("[WS] TLS setup failed: -0x%04x", -result);
		return false;
	}

	mbedtls_ssl_set_bio(&session.ssl, &session.fd, socketSend, socketRecv, nullptr);

	while ((result = mbedtls_ssl_handshake(&session.ssl)) != 0) {
		if (!tlsWouldBlock(result) || osGetTime() >= deadline) {
			Logger::log("[WS] TLS handshake failed: -0x%04x, verify flags 0x%08x", -result,
			            (unsigned)mbedtls_ssl_get_verify_result(&session.ssl));
			return false;
		}
		svcSleepThread(RETRY_NS);
	}
	return true;
}

bool WebSocketClient::upgrade(Tls &session, uint64_t deadline) {
	const std::string request = "GET " + path +
	                            " HTTP/1.1\r\n"
	                            "Host: " +
	                            host +
	                            "\r\n"
	                            "Upgrade: websocket\r\n"
	                            "Connection: Upgrade\r\n"
	                            "Sec-WebSocket-Key: " +
	                            generateWebSocketKey() +
	                            "\r\n"
	                            "Sec-WebSocket-Version: 13\r\n"
	                            "Origin: https://discord.com\r\n"
	                            "User-Agent: " APP_USER_AGENT "\r\n"
	                            "\r\n";

	if (!writeAll(session.ssl, reinterpret_cast<const uint8_t *>(request.data()), request.size(), deadline)) {
		Logger::log("[WS] Failed to send handshake");
		return false;
	}

	std::string response;
	while (response.size() < 4 || response.compare(response.size() - 4, 4, "\r\n\r\n") != 0) {
		unsigned char byte;
		const int result = mbedtls_ssl_read(&session.ssl, &byte, 1);
		if (result == 1 && response.size() < MAX_UPGRADE_RESPONSE) {
			response.push_back(byte);
		} else if (!tlsWouldBlock(result) || osGetTime() >= deadline) {
			Logger::log("[WS] Failed to receive handshake response: -0x%04x", -result);
			return false;
		} else {
			svcSleepThread(RETRY_NS);
		}
	}

	if (response.find(" 101") != 8) {
		Logger::log("[WS] Handshake failed: %s", response.c_str());
		return false;
	}
	return true;
}

bool WebSocketClient::connect(const std::string &url) {
	if (state == WebSocketState::CONNECTED || state == WebSocketState::CONNECTING) {
		Logger::log("WS connect called but already connected/connecting");
		return false;
	}

	Logger::log("WS connect %s", url.c_str());
	if (!parseUrl(url)) {
		Logger::log("WS parseUrl failed");
		return false;
	}

	state = WebSocketState::CONNECTING;

	const uint64_t deadline = osGetTime() + CONNECT_TIMEOUT_MS;
	auto session = std::make_unique<Tls>();
	if (!openSocket(*session, deadline) || !startTls(*session, deadline) || !upgrade(*session, deadline)) {
		state = WebSocketState::DISCONNECTED;
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(ioMutex);
		tls = std::move(session);
		writeBroken = false;
		resetRx();
	}

	Logger::log("WS connected successfully");
	state = WebSocketState::CONNECTED;
	return true;
}

bool WebSocketClient::beginClose() {
	WebSocketState expected = WebSocketState::CONNECTED;
	return state.compare_exchange_strong(expected, WebSocketState::CLOSING);
}

void WebSocketClient::finishClose(int code, const std::string &reason) {
	{
		std::lock_guard<std::mutex> lock(ioMutex);
		tls.reset();
	}

	state = WebSocketState::CLOSED;

	if (onClose) {
		onClose(code, reason);
	}
}

void WebSocketClient::disconnect(int code, const std::string &reason) {
	if (!beginClose()) {
		return;
	}

	const uint8_t payload[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
	{
		std::lock_guard<std::mutex> lock(ioMutex);
		writeFrame(WebSocketOpcode::CLOSE, payload, sizeof(payload), CLOSE_TIMEOUT_MS);
	}
	finishClose(code, reason);
}

void WebSocketClient::fail(const std::string &error) {
	if (!beginClose()) {
		return;
	}
	if (onError) {
		onError(error);
	}
	finishClose(ABNORMAL_CLOSE_CODE, "");
}

bool WebSocketClient::isConnected() const { return state == WebSocketState::CONNECTED; }

WebSocketState WebSocketClient::getState() const { return state; }

bool WebSocketClient::writeFrame(WebSocketOpcode opcode, const void *data, size_t len, uint64_t timeoutMs) {
	if (!tls || writeBroken) {
		return false;
	}

	std::vector<uint8_t> frame;
	frame.reserve(MAX_TX_HEADER + len);
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
			frame.push_back(i < (int)sizeof(size_t) ? (len >> (i * 8)) & 0xFF : 0);
		}
	}

	uint8_t mask[4];
	for (uint8_t &byte : mask) {
		byte = rand() % 256;
		frame.push_back(byte);
	}

	const size_t payloadStart = frame.size();
	frame.resize(payloadStart + len);
	const uint8_t *payload = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < len; i++) {
		frame[payloadStart + i] = payload[i] ^ mask[i & 3];
	}

	writeBroken = !writeAll(tls->ssl, frame.data(), frame.size(), osGetTime() + timeoutMs);
	return !writeBroken;
}

bool WebSocketClient::sendFrame(WebSocketOpcode opcode, const void *data, size_t len) {
	std::lock_guard<std::mutex> lock(ioMutex);
	return state == WebSocketState::CONNECTED && writeFrame(opcode, data, len, WRITE_TIMEOUT_MS);
}

bool WebSocketClient::send(const std::string &message) {
	return sendFrame(WebSocketOpcode::TEXT, message.data(), message.size());
}

bool WebSocketClient::sendBinary(const std::vector<uint8_t> &data) {
	return sendFrame(WebSocketOpcode::BINARY, data.data(), data.size());
}

void WebSocketClient::resetRx() {
	rxHeaderSize = 0;
	rxInPayload = false;
	rxPayloadSize = 0;
	rxPayload.clear();
}

size_t WebSocketClient::rxHeaderNeeded() const {
	if (rxHeaderSize < 2) {
		return 2;
	}
	const uint8_t lengthCode = rxHeader[1] & 0x7F;
	return lengthCode == 127 ? 10 : lengthCode == 126 ? 4 : 2;
}

bool WebSocketClient::beginPayload(std::string &error) {
	if (rxHeader[1] & 0x80) {
		error = "Masked frame from server";
		return false;
	}

	uint64_t length = rxHeader[1] & 0x7F;
	if (rxHeaderSize > 2) {
		length = 0;
		for (size_t i = 2; i < rxHeaderSize; i++) {
			length = (length << 8) | rxHeader[i];
		}
	}

	if (length > LARGE_FRAME && length + FRAME_HEAP_MARGIN > Utils::System::heapFreeBytes()) {
		Logger::log("[WS] Frame of %llu bytes does not fit in memory", length);
		error = "Frame too large";
		return false;
	}

	rxPayload.resize(length);
	rxPayloadSize = 0;
	rxInPayload = true;
	return true;
}

WebSocketClient::ReadStep WebSocketClient::readEnded(int result, std::string &error) const {
	if (tlsWouldBlock(result)) {
		return rxHeaderSize > 0 ? ReadStep::PROGRESS : ReadStep::IDLE;
	}

	char message[32];
	snprintf(message, sizeof(message), "Recv error: -0x%04x", -result);
	error = message;
	return ReadStep::FAILED;
}

WebSocketClient::ReadStep WebSocketClient::readSome(std::string &error) {
	while (!rxInPayload) {
		const size_t needed = rxHeaderNeeded();
		if (rxHeaderSize == needed) {
			if (!beginPayload(error)) {
				return ReadStep::FAILED;
			}
			break;
		}
		const int result = mbedtls_ssl_read(&tls->ssl, rxHeader + rxHeaderSize, needed - rxHeaderSize);
		if (result <= 0) {
			return readEnded(result, error);
		}
		rxHeaderSize += result;
	}

	if (rxPayloadSize < rxPayload.size()) {
		const int result = mbedtls_ssl_read(&tls->ssl, reinterpret_cast<unsigned char *>(&rxPayload[rxPayloadSize]),
		                                    rxPayload.size() - rxPayloadSize);
		if (result <= 0) {
			return readEnded(result, error);
		}
		rxPayloadSize += result;
		if (rxPayloadSize < rxPayload.size()) {
			return ReadStep::PROGRESS;
		}
	}
	return ReadStep::FRAME;
}

void WebSocketClient::dispatch(WebSocketOpcode opcode, std::string &payload) {
	switch (opcode) {
	case WebSocketOpcode::TEXT:
		if (onMessage && !payload.empty()) {
			onMessage(payload);
		}
		break;

	case WebSocketOpcode::BINARY:
		if (payload.empty()) {
			break;
		}
		if (onBinaryMessage) {
			onBinaryMessage(payload);
		} else if (onMessage) {
			onMessage(payload);
		}
		break;

	case WebSocketOpcode::PING:
		sendFrame(WebSocketOpcode::PONG, payload.data(), payload.size());
		break;

	case WebSocketOpcode::CLOSE: {
		int closeCode = 1000;
		if (payload.size() >= 2) {
			closeCode = (static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]);
		}
		if (beginClose()) {
			{
				std::lock_guard<std::mutex> lock(ioMutex);
				writeFrame(WebSocketOpcode::CLOSE, nullptr, 0, CLOSE_TIMEOUT_MS);
			}
			finishClose(closeCode, "");
		}
		break;
	}

	default:
		break;
	}
}

bool WebSocketClient::poll() {
	if (state != WebSocketState::CONNECTED) {
		return false;
	}

	ReadStep step = ReadStep::FAILED;
	WebSocketOpcode opcode = WebSocketOpcode::CONTINUATION;
	std::string payload;
	std::string error = "Write failed";
	{
		std::lock_guard<std::mutex> lock(ioMutex);
		if (!tls) {
			return false;
		}
		if (!writeBroken) {
			step = readSome(error);
		}
		if (step == ReadStep::FRAME) {
			opcode = static_cast<WebSocketOpcode>(rxHeader[0] & 0x0F);
			payload = std::move(rxPayload);
			resetRx();
		}
	}

	if (step == ReadStep::FAILED) {
		fail(error);
		return false;
	}
	if (step == ReadStep::FRAME) {
		dispatch(opcode, payload);
	}
	return step != ReadStep::IDLE;
}

void WebSocketClient::setOnBinaryMessage(MessageCallback callback) { onBinaryMessage = callback; }

void WebSocketClient::setOnMessage(MessageCallback callback) { onMessage = callback; }

void WebSocketClient::setOnError(ErrorCallback callback) { onError = callback; }

void WebSocketClient::setOnClose(CloseCallback callback) { onClose = callback; }

} // namespace Network
