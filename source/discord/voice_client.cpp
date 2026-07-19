#include "discord/voice_client.h"
#include "discord/discord_client.h"
#include "log.h"
#include "utils/json_utils.h"
#include "utils/sound_player.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <cstring>
#include <malloc.h>
#include <mbedtls/gcm.h>
#include <netinet/in.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Discord {

namespace {
constexpr int VOICE_GATEWAY_VERSION = 8;
constexpr int CLOSE_NORMAL = 1000;
constexpr int CLOSE_DAVE_REQUIRED = 4017;
constexpr int OPUS_PAYLOAD_TYPE = 120;
constexpr size_t IP_DISCOVERY_SIZE = 74;
constexpr uint8_t IP_DISCOVERY_BODY_SIZE = 70;
constexpr int RTCP_PT_MIN = 200;
constexpr int RTCP_PT_MAX = 206;
constexpr size_t GCM_TAG_SIZE = 16;
constexpr size_t NONCE_SUFFIX_SIZE = 4;
constexpr int DAVE_PROTOCOL_VERSION = 1;

constexpr int OP_READY = 2;
constexpr int OP_HEARTBEAT = 3;
constexpr int OP_SESSION_DESCRIPTION = 4;
constexpr int OP_SPEAKING = 5;
constexpr int OP_HEARTBEAT_ACK = 6;
constexpr int OP_HELLO = 8;
constexpr int OP_CLIENTS_CONNECT = 11;
constexpr int OP_CLIENT_DISCONNECT = 13;
constexpr int OP_DAVE_PREPARE_TRANSITION = 21;
constexpr int OP_DAVE_EXECUTE_TRANSITION = 22;
constexpr int OP_DAVE_TRANSITION_READY = 23;
constexpr int OP_DAVE_PREPARE_EPOCH = 24;
constexpr int OP_MLS_INVALID_COMMIT_WELCOME = 31;

constexpr uint8_t OP_MLS_EXTERNAL_SENDER = 25;
constexpr uint8_t OP_MLS_KEY_PACKAGE = 26;
constexpr uint8_t OP_MLS_PROPOSALS = 27;
constexpr uint8_t OP_MLS_COMMIT_WELCOME = 28;
constexpr uint8_t OP_MLS_ANNOUNCE_COMMIT = 29;
constexpr uint8_t OP_MLS_WELCOME = 30;

// mbedtls ships AES-GCM but only 12-byte-nonce ChaCha20-Poly1305, so XChaCha20
// would need HChaCha20 written by hand. Prefer GCM while the server offers it.
const char *PREFERRED_MODE = "aead_aes256_gcm_rtpsize";

void logMemory(const char *where) {
	struct mallinfo mi = mallinfo();
	Logger::log("[Mem] %-22s heapUsed=%dKB heapFree=%dKB arena=%dKB osFree=%luKB linear=%luKB", where,
	            mi.uordblks / 1024, mi.fordblks / 1024, mi.arena / 1024,
	            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
	            (unsigned long)(linearSpaceFree() / 1024));
}
} // namespace

VoiceClient &VoiceClient::getInstance() {
	static VoiceClient instance;
	return instance;
}

VoiceClient::~VoiceClient() { disconnect(); }

void VoiceClient::setState(VoiceState s) {
	if (s == VoiceState::ESTABLISHED && state != VoiceState::ESTABLISHED) {
		Utils::SoundPlayer::getInstance().play(Utils::Sound::VOICE_JOIN);
		rosterPrimed = true;
	}
	state = s;
	if (stateCallback) {
		stateCallback(s);
	}
}

void VoiceClient::connect(const std::string &guild, const std::string &channel) {
	disconnect();

	{
		std::lock_guard<std::mutex> lock(mutex);
		guildId = guild;
		channelId = channel;
		haveState = false;
		haveServer = false;
		sessionId.clear();
		token.clear();
		endpoint.clear();
		serverId.clear();
	}
	serverMuted = false;
	serverDeafened = false;
	mutedBeforeDeafen = false;
	rosterPrimed = false;
	audio.setDeafened(false);
	lastSequence = -1;

	DiscordClient &client = DiscordClient::getInstance();
	client.setVoiceStateCallback(
	    [this](const std::string &s, bool srvMute, bool srvDeaf) { onVoiceState(s, srvMute, srvDeaf); });
	client.setVoiceServerCallback(
	    [this](const std::string &t, const std::string &e, const std::string &s) { onVoiceServer(t, e, s); });

	setState(VoiceState::AWAITING_SERVER);
	client.updateVoiceState(guild, channel, false, false);
}

void VoiceClient::markSpeaking(const std::string &userId) {
	std::lock_guard<std::mutex> lock(mutex);
	speakingUntil[userId] = osGetTime() + SPEAKING_HOLD_MS;
}

bool VoiceClient::isSpeaking(const std::string &userId) const {
	std::lock_guard<std::mutex> lock(mutex);
	auto it = speakingUntil.find(userId);
	return it != speakingUntil.end() && osGetTime() < it->second;
}

void VoiceClient::setMuted(bool m) {
	if (capture.isMuted() == m) {
		return;
	}
	capture.setMuted(m);
	Utils::SoundPlayer::getInstance().play(m ? Utils::Sound::MIC_OFF : Utils::Sound::MIC_ON);
	publishVoiceState();
}

void VoiceClient::setDeafened(bool d) {
	if (audio.isDeafened() == d) {
		return;
	}

	if (d) {
		mutedBeforeDeafen = capture.isMuted();
		capture.setMuted(true);
	} else {
		capture.setMuted(mutedBeforeDeafen);
	}

	audio.setDeafened(d);
	Utils::SoundPlayer::getInstance().play(d ? Utils::Sound::HEADPHONE_OFF : Utils::Sound::HEADPHONE_ON);
	publishVoiceState();
}

void VoiceClient::publishVoiceState() {
	std::string guild, channel;
	{
		std::lock_guard<std::mutex> lock(mutex);
		guild = guildId;
		channel = channelId;
	}
	if (channel.empty()) {
		return;
	}
	DiscordClient::getInstance().updateVoiceState(guild, channel, capture.isMuted(), audio.isDeafened());
}

void VoiceClient::onVoiceState(const std::string &session, bool srvMute, bool srvDeaf) {
	if (session != DiscordClient::getInstance().getSessionId()) {
		return;
	}

	serverMuted = srvMute;
	serverDeafened = srvDeaf;
	if (srvMute) {
		capture.setMuted(true);
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		sessionId = session;
		haveState = true;
	}
	tryStartSession();
}

void VoiceClient::onVoiceServer(const std::string &t, const std::string &e, const std::string &s) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		token = t;
		endpoint = e;
		serverId = s;
		haveServer = !t.empty() && !e.empty();
	}
	tryStartSession();
}

void VoiceClient::tryStartSession() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!haveState || !haveServer) {
			return;
		}
	}

	if (worker.joinable()) {
		return;
	}

	stopWorker = false;
	setState(VoiceState::CONNECTING);
	worker = std::thread(&VoiceClient::socketThread, this);
}

void VoiceClient::socketThread() {
	std::string url;
	{
		std::lock_guard<std::mutex> lock(mutex);
		url = "wss://" + endpoint + "/?v=" + std::to_string(VOICE_GATEWAY_VERSION);
	}

	Logger::log("[Voice] Connecting to %s", url.c_str());

	ws.setOnMessage([this](std::string &msg) { handlePayload(msg); });
	ws.setOnBinaryMessage([this](std::string &msg) { handleBinaryPayload(msg); });
	ws.setOnClose([this](int code, const std::string &reason) {
		Logger::log("[Voice] Closed code=%d reason=%s", code, reason.c_str());
		if (code == CLOSE_DAVE_REQUIRED) {
			Logger::log("[Voice] Server requires the DAVE protocol; this client advertises version 0.");
		}
		setState(code == CLOSE_NORMAL ? VoiceState::DISCONNECTED : VoiceState::FAILED);
	});
	ws.setOnError([](const std::string &err) { Logger::log("[Voice] Error: %s", err.c_str()); });

	if (!ws.connect(url)) {
		Logger::log("[Voice] WebSocket connect failed");
		setState(VoiceState::FAILED);
		return;
	}

	setState(VoiceState::IDENTIFYING);
	sendIdentify();

	while (!stopWorker && ws.isConnected()) {
		ws.poll();

		if (heartbeatInterval > 0) {
			uint64_t now = osGetTime();
			if (now - lastHeartbeat >= heartbeatInterval) {
				sendHeartbeat();
				lastHeartbeat = now;
			}
		}

		svcSleepThread(10000000ULL);
	}

	ws.disconnect();
	closeUdp();
}

void VoiceClient::sendIdentify() {
	std::lock_guard<std::mutex> lock(mutex);

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(0);
	writer.Key("d");
	writer.StartObject();
	writer.Key("server_id");
	writer.String(serverId.c_str());
	writer.Key("channel_id");
	writer.String(channelId.c_str());
	writer.Key("user_id");
	writer.String(DiscordClient::getInstance().getCurrentUser().id.c_str());
	writer.Key("session_id");
	writer.String(sessionId.c_str());
	writer.Key("token");
	writer.String(token.c_str());
	writer.Key("video");
	writer.Bool(false);
	writer.Key("max_dave_protocol_version");
	writer.Int(DAVE_PROTOCOL_VERSION);
	writer.EndObject();
	writer.EndObject();

	ws.send(s.GetString());
	Logger::log("[Voice] Sent Identify server=%s channel=%s user=%s session=%s tokenLen=%zu", serverId.c_str(),
	            channelId.c_str(), DiscordClient::getInstance().getCurrentUser().id.c_str(), sessionId.c_str(),
	            token.length());
}

void VoiceClient::sendHeartbeat() {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(OP_HEARTBEAT);
	writer.Key("d");
	writer.StartObject();
	writer.Key("t");
	writer.Int64((int64_t)osGetTime());
	if (lastSequence >= 0) {
		writer.Key("seq_ack");
		writer.Int64(lastSequence);
	}
	writer.EndObject();
	writer.EndObject();

	ws.send(s.GetString());
}

bool VoiceClient::openUdp() {
	closeUdp();

	udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (udpSocket < 0) {
		Logger::log("[Voice] UDP socket() failed");
		return false;
	}

	sockaddr_in server{};
	server.sin_family = AF_INET;
	server.sin_port = htons((uint16_t)udpPort);
	server.sin_addr.s_addr = inet_addr(udpIp.c_str());

	// Connecting a datagram socket fixes the peer so the NAT mapping that IP
	// discovery reports is the one media will arrive on.
	if (::connect(udpSocket, (sockaddr *)&server, sizeof(server)) < 0) {
		Logger::log("[Voice] UDP connect() to %s:%d failed", udpIp.c_str(), udpPort);
		closeUdp();
		return false;
	}

	Logger::log("[Voice] UDP socket connected to %s:%d", udpIp.c_str(), udpPort);
	return true;
}

void VoiceClient::closeUdp() {
	if (udpSocket >= 0) {
		close(udpSocket);
		udpSocket = -1;
	}
}

bool VoiceClient::discoverExternalAddress() {
	uint8_t packet[IP_DISCOVERY_SIZE];
	memset(packet, 0, sizeof(packet));
	packet[0] = 0x00;
	packet[1] = 0x01;
	packet[2] = 0x00;
	packet[3] = IP_DISCOVERY_BODY_SIZE;
	packet[4] = (uint8_t)(ssrc >> 24);
	packet[5] = (uint8_t)(ssrc >> 16);
	packet[6] = (uint8_t)(ssrc >> 8);
	packet[7] = (uint8_t)ssrc;

	for (int attempt = 0; attempt < 5 && !stopWorker; attempt++) {
		if (send(udpSocket, packet, sizeof(packet), 0) < 0) {
			Logger::log("[Voice] IP discovery send failed");
			return false;
		}

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(udpSocket, &readSet);
		timeval timeout{};
		timeout.tv_sec = 1;

		int ready = select(udpSocket + 1, &readSet, nullptr, nullptr, &timeout);
		if (ready <= 0) {
			continue;
		}

		uint8_t response[128];
		int received = recv(udpSocket, response, sizeof(response), 0);
		if (received < (int)IP_DISCOVERY_SIZE) {
			continue;
		}
		if (response[0] != 0x00 || response[1] != 0x02) {
			continue;
		}

		char address[65];
		memcpy(address, response + 8, 64);
		address[64] = '\0';

		externalIp = address;
		externalPort = (response[72] << 8) | response[73];
		Logger::log("[Voice] IP discovery: %s:%d", externalIp.c_str(), externalPort);
		return true;
	}

	Logger::log("[Voice] IP discovery timed out");
	return false;
}

void VoiceClient::sendSelectProtocol() {
	std::string mode = PREFERRED_MODE;
	bool offered = false;
	for (const std::string &m : serverModes) {
		if (m == mode) {
			offered = true;
			break;
		}
	}
	if (!offered) {
		Logger::log("[Voice] %s not offered by server; cannot negotiate", mode.c_str());
		setState(VoiceState::FAILED);
		return;
	}

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(1);
	writer.Key("d");
	writer.StartObject();
	writer.Key("protocol");
	writer.String("udp");
	writer.Key("data");
	writer.StartObject();
	writer.Key("address");
	writer.String(externalIp.c_str());
	writer.Key("port");
	writer.Int(externalPort);
	writer.Key("mode");
	writer.String(mode.c_str());
	writer.EndObject();
	writer.Key("codecs");
	writer.StartArray();
	writer.StartObject();
	writer.Key("name");
	writer.String("opus");
	writer.Key("type");
	writer.String("audio");
	writer.Key("priority");
	writer.Int(1000);
	writer.Key("payload_type");
	writer.Int(OPUS_PAYLOAD_TYPE);
	writer.Key("encode");
	writer.Bool(false);
	writer.Key("decode");
	writer.Bool(true);
	writer.EndObject();
	writer.EndArray();
	writer.EndObject();
	writer.EndObject();

	ws.send(s.GetString());
	setState(VoiceState::SELECTING_PROTOCOL);
	Logger::log("[Voice] Sent Select Protocol mode=%s addr=%s:%d", mode.c_str(), externalIp.c_str(), externalPort);
}

void VoiceClient::handleRtpPacket(uint8_t *packet, size_t len) {
	if (len < 12 + GCM_TAG_SIZE + NONCE_SUFFIX_SIZE) {
		return;
	}
	if ((packet[0] >> 6) != 2) {
		return;
	}

	uint8_t payloadType = packet[1] & 0x7F;
	if (payloadType >= RTCP_PT_MIN && payloadType <= RTCP_PT_MAX) {
		return;
	}
	if (payloadType != OPUS_PAYLOAD_TYPE) {
		return;
	}

	int csrcCount = packet[0] & 0x0F;
	bool hasExtension = (packet[0] & 0x10) != 0;
	size_t headerLen = 12 + (size_t)csrcCount * 4 + (hasExtension ? 4 : 0);
	if (len < headerLen + GCM_TAG_SIZE + NONCE_SUFFIX_SIZE) {
		return;
	}

	uint32_t ssrc =
	    ((uint32_t)packet[8] << 24) | ((uint32_t)packet[9] << 16) | ((uint32_t)packet[10] << 8) | (uint32_t)packet[11];

	const uint8_t *suffix = packet + len - NONCE_SUFFIX_SIZE;
	uint8_t iv[12];
	memset(iv, 0, sizeof(iv));
	memcpy(iv, suffix, NONCE_SUFFIX_SIZE);

	size_t bodyLen = len - headerLen - NONCE_SUFFIX_SIZE;
	if (bodyLen < GCM_TAG_SIZE) {
		return;
	}
	size_t cipherLen = bodyLen - GCM_TAG_SIZE;
	const uint8_t *cipher = packet + headerLen;
	const uint8_t *tag = cipher + cipherLen;

	uint8_t *plain = plainBuffer;
	if (cipherLen > sizeof(plainBuffer) || !gcm) {
		return;
	}

	int rc =
	    mbedtls_gcm_auth_decrypt(gcm, cipherLen, iv, sizeof(iv), packet, headerLen, tag, GCM_TAG_SIZE, cipher, plain);

	if (rc != 0) {
		decryptFailures++;
		if (decryptFailures <= 5 || decryptFailures % 200 == 0) {
			Logger::log("[Voice] decrypt failed (%d) count=%lu", rc, (unsigned long)decryptFailures);
		}
		return;
	}

	const uint8_t *opusData = plain;
	size_t opusLen = cipherLen;

	// With rtpsize only the extension preamble is authenticated in the clear;
	// the elements themselves sit at the front of the decrypted body.
	if (hasExtension) {
		size_t extWords = ((size_t)packet[headerLen - 2] << 8) | packet[headerLen - 1];
		size_t extBytes = extWords * 4;
		if (extBytes > opusLen) {
			return;
		}
		opusData += extBytes;
		opusLen -= extBytes;
	}

	if (opusLen == 0) {
		return;
	}

	packetsDecoded++;
	if (packetsDecoded == 1) {
		Logger::log("[Voice] First media packet decrypted, ssrc=%lu bytes=%zu", (unsigned long)ssrc, opusLen);
	}

	if (daveVersion != 0) {
		std::string userId;
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto it = ssrcToUser.find(ssrc);
			if (it == ssrcToUser.end()) {
				return;
			}
			userId = it->second;
		}

		size_t written = 0;
		if (!dave.decryptOpus(userId, opusData, opusLen, daveFrameBuffer, sizeof(daveFrameBuffer), &written) ||
		    written == 0) {
			daveDecryptFailures++;
			if (daveDecryptFailures <= 5 || daveDecryptFailures % 200 == 0) {
				Logger::log("[Voice] DAVE decrypt failed count=%lu", (unsigned long)daveDecryptFailures);
			}
			return;
		}
		markSpeaking(userId);
		audio.pushOpus(ssrc, daveFrameBuffer, written);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = ssrcToUser.find(ssrc);
		if (it != ssrcToUser.end()) {
			speakingUntil[it->second] = osGetTime() + SPEAKING_HOLD_MS;
		}
	}
	audio.pushOpus(ssrc, opusData, opusLen);
}

void VoiceClient::sendSpeaking(bool speaking) {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(5);
	writer.Key("d");
	writer.StartObject();
	writer.Key("speaking");
	writer.Int(speaking ? 1 : 0);
	writer.Key("delay");
	writer.Int(0);
	writer.Key("ssrc");
	writer.Uint(ssrc);
	writer.EndObject();
	writer.EndObject();

	ws.send(s.GetString());
	Logger::log("[Voice] Speaking=%d ssrc=%lu", speaking ? 1 : 0, (unsigned long)ssrc);
}

void VoiceClient::sendAudioFrame(const uint8_t *opusData, size_t opusLen) {
	if (!gcm || udpSocket < 0) {
		return;
	}

	if (daveVersion != 0) {
		size_t written = 0;
		if (!dave.encryptOpus(ssrc, opusData, opusLen, daveFrameBuffer, sizeof(daveFrameBuffer), &written) ||
		    written == 0) {
			return;
		}
		opusData = daveFrameBuffer;
		opusLen = written;
	}

	uint8_t *packet = sendBuffer;
	packet[0] = 0x80;
	packet[1] = (uint8_t)OPUS_PAYLOAD_TYPE;
	packet[2] = (uint8_t)(sendSequence >> 8);
	packet[3] = (uint8_t)sendSequence;
	packet[4] = (uint8_t)(sendTimestamp >> 24);
	packet[5] = (uint8_t)(sendTimestamp >> 16);
	packet[6] = (uint8_t)(sendTimestamp >> 8);
	packet[7] = (uint8_t)sendTimestamp;
	packet[8] = (uint8_t)(ssrc >> 24);
	packet[9] = (uint8_t)(ssrc >> 16);
	packet[10] = (uint8_t)(ssrc >> 8);
	packet[11] = (uint8_t)ssrc;

	if (12 + opusLen + GCM_TAG_SIZE + NONCE_SUFFIX_SIZE > sizeof(sendBuffer)) {
		return;
	}

	uint8_t iv[12];
	memset(iv, 0, sizeof(iv));
	iv[0] = (uint8_t)(sendNonce >> 24);
	iv[1] = (uint8_t)(sendNonce >> 16);
	iv[2] = (uint8_t)(sendNonce >> 8);
	iv[3] = (uint8_t)sendNonce;

	uint8_t *cipher = packet + 12;
	uint8_t *tag = cipher + opusLen;

	if (mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, opusLen, iv, sizeof(iv), packet, 12, opusData, cipher,
	                              GCM_TAG_SIZE, tag) != 0) {
		return;
	}

	uint8_t *suffix = tag + GCM_TAG_SIZE;
	suffix[0] = iv[0];
	suffix[1] = iv[1];
	suffix[2] = iv[2];
	suffix[3] = iv[3];

	size_t total = 12 + opusLen + GCM_TAG_SIZE + NONCE_SUFFIX_SIZE;
	send(udpSocket, packet, total, 0);

	sendNonce++;
	sendSequence++;
	// Opus RTP always uses a 48kHz clock regardless of the encoder rate.
	sendTimestamp += 960;
	packetsSent++;
	if (packetsSent == 1) {
		Logger::log("[Voice] First audio frame sent (%zu bytes opus)", opusLen);
	}
}

void VoiceClient::mediaThreadEntry(void *arg) { static_cast<VoiceClient *>(arg)->mediaThread(); }

void VoiceClient::mediaThread() {
	Logger::log("[Voice] Media thread started");

	while (!stopMedia) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(udpSocket, &readSet);
		timeval timeout{};
		timeout.tv_usec = 10000;

		int ready = select(udpSocket + 1, &readSet, nullptr, nullptr, &timeout);
		if (ready > 0) {
			int received = recv(udpSocket, recvBuffer, sizeof(recvBuffer), 0);
			if (received > 0) {
				handleRtpPacket(recvBuffer, (size_t)received);
			}
		}

		audio.pump();

		uint8_t frame[512];
		int encoded = capture.poll(frame, sizeof(frame));
		if (encoded > 0) {
			if (daveVersion != 0 && !dave.hasGroup()) {
				if (!daveWaitLogged) {
					Logger::log("[Voice] Holding audio until the DAVE group is established");
					daveWaitLogged = true;
				}
				continue;
			}

			bool active = capture.lastPeak() >= SPEAKING_PEAK_THRESHOLD;
			if (active) {
				markSpeaking(DiscordClient::getInstance().getCurrentUser().id);
			}
			if (active != speakingSent) {
				sendSpeaking(active);
				speakingSent = active;
			}
			sendAudioFrame(frame, (size_t)encoded);
		}
	}

	Logger::log("[Voice] Media thread stopped (decoded=%lu failures=%lu sent=%lu)", (unsigned long)packetsDecoded,
	            (unsigned long)decryptFailures, (unsigned long)packetsSent);
}

std::set<std::string> VoiceClient::recognizedUsers() const {
	std::lock_guard<std::mutex> lock(mutex);
	std::set<std::string> users = roster;
	users.insert(DiscordClient::getInstance().getCurrentUser().id);
	return users;
}

void VoiceClient::recoverFromInvalidGroup() {
	if (invalidGroupRetries >= MAX_INVALID_GROUP_RETRIES) {
		Logger::log("[Voice] Giving up after %d re-add attempts", invalidGroupRetries);
		return;
	}
	invalidGroupRetries++;

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(OP_MLS_INVALID_COMMIT_WELCOME);
	writer.Key("d");
	writer.StartObject();
	writer.Key("transition_id");
	writer.Int(currentTransitionId);
	writer.EndObject();
	writer.EndObject();
	ws.send(s.GetString());

	std::string groupIdStr;
	{
		std::lock_guard<std::mutex> lock(mutex);
		groupIdStr = channelId;
	}
	uint64_t groupId = strtoull(groupIdStr.c_str(), nullptr, 10);
	if (!dave.init((uint16_t)daveVersion, groupId, DiscordClient::getInstance().getCurrentUser().id)) {
		Logger::log("[Voice] Re-init failed: %s", dave.lastError().c_str());
		return;
	}

	if (externalSenderPackage.empty()) {
		Logger::log("[Voice] No stored external sender; cannot rebuild the group");
		return;
	}
	dave.setExternalSender(externalSenderPackage);

	auto kp = dave.keyPackage();
	if (kp.empty()) {
		Logger::log("[Voice] Re-add produced an empty key package");
		return;
	}
	sendBinary(OP_MLS_KEY_PACKAGE, kp);

	Logger::log("[Voice] Requested re-add (attempt %d, transition %d)", invalidGroupRetries, currentTransitionId);
}

void VoiceClient::sendBinary(uint8_t opcode, const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> frame;
	frame.reserve(1 + payload.size());
	frame.push_back(opcode);
	frame.insert(frame.end(), payload.begin(), payload.end());
	const bool sent = ws.sendBinary(frame);
	Logger::log("[Voice] -> binary op=%u len=%zu sent=%d", opcode, payload.size(), sent ? 1 : 0);
}

void VoiceClient::handleBinaryPayload(const std::string &message) {
	if (message.size() < 3) {
		return;
	}

	const uint8_t *data = (const uint8_t *)message.data();
	lastSequence = ((int64_t)data[0] << 8) | data[1];
	const uint8_t opcode = data[2];
	const std::vector<uint8_t> payload(data + 3, data + message.size());

	Logger::log("[Voice] <- binary op=%u len=%zu", opcode, payload.size());

	switch (opcode) {
	case OP_MLS_EXTERNAL_SENDER:
		externalSenderPackage = payload;
		dave.setExternalSender(payload);
		if (davePending) {
			auto kp = dave.keyPackage();
			if (!kp.empty()) {
				sendBinary(OP_MLS_KEY_PACKAGE, kp);
			}
		}
		break;

	case OP_MLS_PROPOSALS: {
		std::vector<uint8_t> commitWelcome;
		if (dave.processProposals(payload, recognizedUsers(), commitWelcome) && !commitWelcome.empty()) {
			sendBinary(OP_MLS_COMMIT_WELCOME, commitWelcome);
		}
		break;
	}

	// Only these two carry a transition id ahead of the MLS data; feeding it to
	// MLS makes every parse fail.
	case OP_MLS_WELCOME: {
		if (payload.size() < 2) {
			break;
		}
		currentTransitionId = ((int)payload[0] << 8) | payload[1];
		const std::vector<uint8_t> welcome(payload.begin() + 2, payload.end());

		if (dave.processWelcome(welcome, recognizedUsers())) {
			dave.refreshKeyRatchets(recognizedUsers());
			Logger::log("[Voice] DAVE group established via welcome (transition %d)", currentTransitionId);
			logMemory("after welcome");
		} else {
			recoverFromInvalidGroup();
		}
		break;
	}

	case OP_MLS_ANNOUNCE_COMMIT: {
		if (payload.size() < 2) {
			break;
		}
		currentTransitionId = ((int)payload[0] << 8) | payload[1];
		const std::vector<uint8_t> commit(payload.begin() + 2, payload.end());

		if (dave.processCommit(commit)) {
			dave.refreshKeyRatchets(recognizedUsers());
			Logger::log("[Voice] DAVE group established via commit (transition %d)", currentTransitionId);
			logMemory("after commit");
		} else {
			recoverFromInvalidGroup();
		}
		break;
	}

	default:
		break;
	}
}

void VoiceClient::handlePayload(const std::string &message) {
	Logger::log("[Voice] <- %.300s", message.c_str());

	rapidjson::Document doc;
	doc.Parse(message.c_str());
	if (doc.HasParseError() || !doc.IsObject()) {
		return;
	}

	int op = Utils::Json::getInt(doc, "op", -1);

	if (doc.HasMember("seq") && doc["seq"].IsInt64()) {
		lastSequence = doc["seq"].GetInt64();
	}

	switch (op) {
	case OP_HELLO: {
		if (doc.HasMember("d") && doc["d"].IsObject()) {
			heartbeatInterval = Utils::Json::getUint64(doc["d"], "heartbeat_interval");
		}
		lastHeartbeat = osGetTime();
		Logger::log("[Voice] Hello, heartbeat interval %llu ms", heartbeatInterval);
		break;
	}
	case OP_READY: {
		if (!doc.HasMember("d") || !doc["d"].IsObject()) {
			break;
		}
		const rapidjson::Value &d = doc["d"];
		ssrc = (uint32_t)Utils::Json::getUint64(d, "ssrc");
		udpIp = Utils::Json::getString(d, "ip");
		udpPort = Utils::Json::getInt(d, "port");

		serverModes.clear();
		if (d.HasMember("modes") && d["modes"].IsArray()) {
			for (const auto &m : d["modes"].GetArray()) {
				if (m.IsString()) {
					serverModes.push_back(m.GetString());
				}
			}
		}

		std::string modeList;
		for (const std::string &m : serverModes) {
			modeList += (modeList.empty() ? "" : ", ") + m;
		}
		Logger::log("[Voice] Ready ssrc=%lu udp=%s:%d modes=[%s]", (unsigned long)ssrc, udpIp.c_str(), udpPort,
		            modeList.c_str());
		setState(VoiceState::READY);

		if (openUdp() && discoverExternalAddress()) {
			sendSelectProtocol();
		} else {
			setState(VoiceState::FAILED);
		}
		break;
	}
	case OP_SESSION_DESCRIPTION: {
		if (!doc.HasMember("d") || !doc["d"].IsObject()) {
			break;
		}
		const rapidjson::Value &d = doc["d"];
		selectedMode = Utils::Json::getString(d, "mode");

		secretKey.clear();
		if (d.HasMember("secret_key") && d["secret_key"].IsArray()) {
			for (const auto &b : d["secret_key"].GetArray()) {
				if (b.IsInt()) {
					secretKey.push_back((uint8_t)b.GetInt());
				}
			}
		}

		daveVersion = Utils::Json::getInt(d, "dave_protocol_version", 0);
		Logger::log("[Voice] Session Description mode=%s keyBytes=%zu dave=%d audio=%s", selectedMode.c_str(),
		            secretKey.size(), daveVersion, Utils::Json::getString(d, "audio_codec").c_str());

		if (secretKey.size() != 32) {
			Logger::log("[Voice] Unexpected secret key length");
			setState(VoiceState::FAILED);
			break;
		}
		if (daveVersion != 0) {
			// The MLS group is keyed by the voice channel, not the guild.
			uint64_t groupId = strtoull(channelId.c_str(), nullptr, 10);
			if (!dave.init((uint16_t)daveVersion, groupId, DiscordClient::getInstance().getCurrentUser().id)) {
				Logger::log("[Voice] DAVE init failed: %s", dave.lastError().c_str());
				setState(VoiceState::FAILED);
				break;
			}
			davePending = true;
			daveWaitLogged = false;
			invalidGroupRetries = 0;
		}

		if (gcm) {
			mbedtls_gcm_free(gcm);
			delete gcm;
			gcm = nullptr;
		}
		gcm = new mbedtls_gcm_context;
		mbedtls_gcm_init(gcm);
		if (mbedtls_gcm_setkey(gcm, MBEDTLS_CIPHER_ID_AES, secretKey.data(), 256) != 0) {
			Logger::log("[Voice] gcm_setkey failed");
			setState(VoiceState::FAILED);
			break;
		}

		if (!audio.start()) {
			setState(VoiceState::FAILED);
			break;
		}

		capture.setMuted(true);
		if (!capture.start()) {
			Logger::log("[Voice] Capture unavailable; receive only");
		}
		publishVoiceState();
		speakingSent = false;
		sendNonce = 0;
		sendSequence = 0;
		sendTimestamp = 0;
		packetsSent = 0;

		stopMedia = false;
		if (!media) {
			media = threadCreate(mediaThreadEntry, this, MEDIA_STACK_SIZE, 0x3F, 0, false);
			if (!media) {
				Logger::log("[Voice] Media thread creation failed");
				setState(VoiceState::FAILED);
				break;
			}
		}

		logMemory("established");
		setState(VoiceState::ESTABLISHED);
		break;
	}
	case OP_SPEAKING: {
		if (!doc.HasMember("d") || !doc["d"].IsObject()) {
			break;
		}
		const rapidjson::Value &d = doc["d"];
		uint32_t speakerSsrc = (uint32_t)Utils::Json::getUint64(d, "ssrc");
		std::string userId = Utils::Json::getString(d, "user_id");
		if (speakerSsrc != 0 && !userId.empty()) {
			std::lock_guard<std::mutex> lock(mutex);
			ssrcToUser[speakerSsrc] = userId;
		}
		break;
	}
	case OP_CLIENTS_CONNECT: {
		if (!doc.HasMember("d") || !doc["d"].IsObject()) {
			break;
		}
		const rapidjson::Value &d = doc["d"];
		if (d.HasMember("user_ids") && d["user_ids"].IsArray()) {
			std::lock_guard<std::mutex> lock(mutex);
			bool joined = false;
			for (const auto &u : d["user_ids"].GetArray()) {
				if (u.IsString() && roster.insert(u.GetString()).second) {
					joined = true;
				}
			}
			if (joined && rosterPrimed) {
				Utils::SoundPlayer::getInstance().play(Utils::Sound::VOICE_JOIN);
			}
			Logger::log("[Voice] Roster now %zu members", roster.size());
			logMemory("roster change");
		}
		break;
	}
	case OP_CLIENT_DISCONNECT: {
		if (!doc.HasMember("d") || !doc["d"].IsObject()) {
			break;
		}
		std::string userId = Utils::Json::getString(doc["d"], "user_id");
		std::lock_guard<std::mutex> lock(mutex);
		if (roster.erase(userId) > 0) {
			Utils::SoundPlayer::getInstance().play(Utils::Sound::VOICE_PEER_LEFT);
		}
		for (auto it = ssrcToUser.begin(); it != ssrcToUser.end();) {
			if (it->second == userId) {
				audio.dropSpeaker(it->first);
				it = ssrcToUser.erase(it);
			} else {
				++it;
			}
		}
		break;
	}
	case OP_DAVE_PREPARE_TRANSITION:
	case OP_DAVE_PREPARE_EPOCH: {
		int transitionId = 0;
		if (doc.HasMember("d") && doc["d"].IsObject()) {
			transitionId = Utils::Json::getInt(doc["d"], "transition_id", 0);
		}
		currentTransitionId = transitionId;
		rapidjson::StringBuffer s;
		rapidjson::Writer<rapidjson::StringBuffer> writer(s);
		writer.StartObject();
		writer.Key("op");
		writer.Int(OP_DAVE_TRANSITION_READY);
		writer.Key("d");
		writer.StartObject();
		writer.Key("transition_id");
		writer.Int(transitionId);
		writer.EndObject();
		writer.EndObject();
		ws.send(s.GetString());
		Logger::log("[Voice] DAVE transition %d ready", transitionId);
		break;
	}
	case OP_DAVE_EXECUTE_TRANSITION: {
		dave.refreshKeyRatchets(recognizedUsers());
		Logger::log("[Voice] DAVE transition executed");
		break;
	}
	case OP_MLS_ANNOUNCE_COMMIT:
		if (doc.HasMember("d") && doc["d"].IsObject()) {
			Logger::log("[Voice] MLS commit announced");
		}
		break;
	case OP_HEARTBEAT_ACK:
		break;
	default:
		Logger::log("[Voice] Opcode %d", op);
		break;
	}
}

void VoiceClient::disconnect() {
	if (state == VoiceState::ESTABLISHED) {
		Utils::SoundPlayer::getInstance().play(Utils::Sound::VOICE_LEFT);
	}

	stopMedia = true;
	if (media) {
		threadJoin(media, U64_MAX);
		threadFree(media);
		media = nullptr;
	}
	capture.stop();
	audio.stop();

	stopWorker = true;
	if (worker.joinable()) {
		worker.join();
	}

	if (gcm) {
		mbedtls_gcm_free(gcm);
		delete gcm;
		gcm = nullptr;
	}
	ssrcToUser.clear();

	DiscordClient &client = DiscordClient::getInstance();
	client.setVoiceStateCallback(nullptr);
	client.setVoiceServerCallback(nullptr);

	std::string guild;
	{
		std::lock_guard<std::mutex> lock(mutex);
		guild = guildId;
	}
	if (!guild.empty() || !channelId.empty()) {
		client.updateVoiceState(guild, "", false, false);
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		guildId.clear();
		channelId.clear();
		roster.clear();
		speakingUntil.clear();
		externalSenderPackage.clear();
		haveState = false;
		haveServer = false;
	}

	heartbeatInterval = 0;
	if (state != VoiceState::FAILED) {
		setState(VoiceState::DISCONNECTED);
	}
}

} // namespace Discord
