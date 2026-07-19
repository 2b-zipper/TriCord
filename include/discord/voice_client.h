#ifndef VOICE_CLIENT_H
#define VOICE_CLIENT_H

#include "discord/dave_session.h"
#include "discord/voice_audio.h"
#include "discord/voice_capture.h"
#include "network/websocket_client.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <vector>

struct mbedtls_gcm_context;

namespace Discord {

enum class VoiceState {
	DISCONNECTED,
	AWAITING_SERVER,
	CONNECTING,
	IDENTIFYING,
	READY,
	SELECTING_PROTOCOL,
	ESTABLISHED,
	FAILED,
};

class VoiceClient {
  public:
	static VoiceClient &getInstance();

	void connect(const std::string &guildId, const std::string &channelId);
	void disconnect();

	VoiceState getState() const { return state; }

	void setStateCallback(std::function<void(VoiceState)> cb) { stateCallback = cb; }

  private:
	VoiceClient() = default;
	~VoiceClient();

	VoiceClient(const VoiceClient &) = delete;
	VoiceClient &operator=(const VoiceClient &) = delete;

	void onVoiceState(const std::string &session, bool srvMute, bool srvDeaf);
	void publishVoiceState();
	void onVoiceServer(const std::string &token, const std::string &endpoint, const std::string &serverId);
	void tryStartSession();
	void socketThread();
	void handlePayload(const std::string &message);
	void sendIdentify();
	void sendHeartbeat();
	void setState(VoiceState s);

	bool openUdp();
	void closeUdp();
	bool discoverExternalAddress();
	void sendSelectProtocol();
	void mediaThread();
	static void mediaThreadEntry(void *arg);
	void handleRtpPacket(uint8_t *packet, size_t len);
	void sendAudioFrame(const uint8_t *opusData, size_t opusLen);
	void sendSpeaking(bool speaking);
	void handleBinaryPayload(const std::string &message);
	void sendBinary(uint8_t opcode, const std::vector<uint8_t> &payload);
	std::set<std::string> recognizedUsers() const;
	void recoverFromInvalidGroup();

	Network::WebSocketClient ws;
	std::thread worker;
	static constexpr size_t MEDIA_STACK_SIZE = 64 * 1024;
	Thread media = nullptr;
	std::atomic<bool> stopWorker{false};
	std::atomic<bool> stopMedia{false};
	VoiceAudio audio;
	VoiceCapture capture;
	std::atomic<VoiceState> state{VoiceState::DISCONNECTED};
	std::atomic<bool> serverMuted{false};
	std::atomic<bool> serverDeafened{false};
	bool mutedBeforeDeafen = false;

	mutable std::mutex mutex;
	std::string guildId;
	std::string channelId;
	std::string sessionId;
	std::string token;
	std::string endpoint;
	std::string serverId;
	bool haveState = false;
	bool haveServer = false;

	uint64_t heartbeatInterval = 0;
	uint64_t lastHeartbeat = 0;
	int64_t lastSequence = -1;
	uint32_t ssrc = 0;
	std::string udpIp;
	int udpPort = 0;
	std::vector<std::string> serverModes;

	int udpSocket = -1;
	std::string externalIp;
	int externalPort = 0;
	std::string selectedMode;
	std::vector<uint8_t> secretKey;
	mbedtls_gcm_context *gcm = nullptr;
	std::map<uint32_t, std::string> ssrcToUser;
	std::set<std::string> roster;
	// Discord clients stop sending audio instead of reliably sending a
	// speaking:0, so activity is tracked by packet arrival and expires.
	std::map<std::string, uint64_t> speakingUntil;
	static constexpr uint64_t SPEAKING_HOLD_MS = 400;
	static constexpr int SPEAKING_PEAK_THRESHOLD = 1200;
	void markSpeaking(const std::string &userId);
	std::vector<uint8_t> externalSenderPackage;
	uint32_t packetsDecoded = 0;
	uint32_t decryptFailures = 0;
	uint32_t daveDecryptFailures = 0;
	uint8_t recvBuffer[1600];
	uint8_t plainBuffer[1500];
	uint8_t daveFrameBuffer[1500];
	uint8_t sendBuffer[1500];
	DaveSession dave;
	int daveVersion = 0;
	bool davePending = false;
	bool daveWaitLogged = false;
	int currentTransitionId = 0;
	int invalidGroupRetries = 0;
	static constexpr int MAX_INVALID_GROUP_RETRIES = 3;
	uint32_t packetsSent = 0;
	uint32_t sendNonce = 0;
	uint16_t sendSequence = 0;
	uint32_t sendTimestamp = 0;
	bool speakingSent = false;

  public:
	void setMuted(bool m);
	bool isMuted() const { return capture.isMuted(); }
	void setDeafened(bool d);
	bool isDeafened() const { return audio.isDeafened(); }
	bool isSpeaking(const std::string &userId) const;
	bool isServerMuted() const { return serverMuted; }
	bool isServerDeafened() const { return serverDeafened; }

	std::function<void(VoiceState)> stateCallback;
};

} // namespace Discord

#endif // VOICE_CLIENT_H
