#include "discord/discord_client.h"
#include "core/config.h"
#include "discord/avatar_cache.h"
#include "core/i18n.h"
#include "log.h"
#include "network/http_client.h"
#include "network/network_manager.h"
#include "utils/gif_decoder.h"
#include "utils/image_utils.h"
#include "utils/json_utils.h"
#include "utils/message_utils.h"
#include "utils/redraw.h"
#include "utils/system_utils.h"
#include "utils/sound_player.h"
#include <3ds.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <memory>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>

namespace Discord {

namespace {
// Closing with anything but 1000 or 1001 keeps the session alive for a resume.
constexpr int RESUMABLE_CLOSE_CODE = 4000;
// Heap kept free for everything the app does after login.
constexpr size_t GUILD_DATA_RESERVE = 4 * 1024 * 1024;
constexpr int INTERACTION_MESSAGE_COMPONENT = 3;
constexpr int INTERACTION_MODAL_SUBMIT = 5;
constexpr size_t MAX_EPHEMERAL_PER_CHANNEL = 20;

bool hasRoomForGuildData() { return Utils::System::heapFreeBytes() >= GUILD_DATA_RESERVE; }

bool isStreamedEvent(const std::string &event) {
	return event == "READY" || event == "READY_SUPPLEMENTAL" || event == "GUILD_CREATE";
}

// False when the text ends before "t", which for a prefix means it comes later.
bool peekEventName(const char *json, size_t size, std::string &event) {
	Network::JsonInputStream stream(json, size);
	JsonPull p(stream);
	if (!p.next() || p.token() != JsonPull::Token::START_OBJECT) {
		return false;
	}

	std::string key;
	while (p.nextKey(key)) {
		if (key == "t") {
			if (!p.readValue()) {
				return false;
			}
			event = p.token() == JsonPull::Token::STRING ? p.text() : "";
			return true;
		}
		if (!p.skipValue()) {
			return false;
		}
	}
	event.clear();
	return p.ok();
}

std::string statusToString(UserStatus status) {
	switch (status) {
	case UserStatus::ONLINE:
		return "online";
	case UserStatus::IDLE:
		return "idle";
	case UserStatus::DND:
		return "dnd";
	case UserStatus::INVISIBLE:
		return "invisible";
	default:
		return "online";
	}
}

UserStatus stringToStatus(const std::string &s) {
	if (s == "online") {
		return UserStatus::ONLINE;
	}
	if (s == "idle") {
		return UserStatus::IDLE;
	}
	if (s == "dnd") {
		return UserStatus::DND;
	}
	if (s == "invisible") {
		return UserStatus::INVISIBLE;
	}
	if (s == "offline") {
		return UserStatus::OFFLINE;
	}
	return UserStatus::UNKNOWN;
}

std::string urlEncode(const std::string &value) {
	std::ostringstream escaped;
	escaped.fill('0');
	escaped << std::hex;

	for (auto i = value.begin(), n = value.end(); i != n; ++i) {
		std::string::value_type c = (*i);

		if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
			escaped << c;
			continue;
		}

		escaped << std::uppercase << '%' << std::setw(2) << int((unsigned char)c);
	}

	return escaped.str();
}

// Cloudflare's 429s have no JSON body, and one without Retry-After must not be retried.
std::optional<double> retryAfterSeconds(const Network::HttpResponse &resp) {
	rapidjson::Document doc;
	doc.Parse(resp.body.c_str());
	if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("retry_after") && doc["retry_after"].IsNumber()) {
		return doc["retry_after"].GetDouble();
	}
	if (!resp.retryAfter.empty()) {
		return strtod(resp.retryAfter.c_str(), nullptr);
	}
	return std::nullopt;
}
} // namespace

DiscordClient &DiscordClient::getInstance() {
	static DiscordClient instance;
	return instance;
}

void DiscordClient::init() {}

DiscordClient::DiscordClient()
    : state(ConnectionState::DISCONNECTED), heartbeatInterval(0), lastHeartbeat(0), waitingForHeartbeatAck(false),
      hasReceivedHello(false), sessionId(""), lastSequence(0), isConnecting(false), stopWorker(false) {

	workerThread.start([this] { workerLoop(); }, 1);
}

DiscordClient::~DiscordClient() { shutdown(); }

void DiscordClient::shutdown() {
	Logger::log("DiscordClient::shutdown starting...");
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		stopWorker = true;
	}
	queueCv.notify_all();
	if (workerThread.joinable()) {
		workerThread.join();
	}
	disconnect();
	Logger::log("DiscordClient::shutdown complete");
}

bool DiscordClient::connect(const std::string &token) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (state != ConnectionState::DISCONNECTED && state != ConnectionState::DISCONNECTED_ERROR) {
		Logger::log("Connect called but state is %d", (int)state.load());
		return false;
	}

	if (isConnecting) {
		Logger::log("Connect called but already in progress");
		return false;
	}

	this->token = token;
	authFailed.store(false);
	isConnecting = true;
	setState(ConnectionState::CONNECTING, "Starting network thread...");

	if (networkThread.joinable()) {
		networkThread.join();
	}

	{
		std::lock_guard<std::mutex> lock(sendQueueMutex);
		sendQueue.clear();
	}

	networkThread.start([this, token] { runNetworkThread(token); }, 1);
	return true;
}

void DiscordClient::logout() {
	disconnect();
	invalidateSession();
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	guilds.clear();
	folders.clear();
	ephemeralMessages.clear();
	currentUser = User();
	self = User();
	token.clear();
	selectedGuildId.clear();
	selectedChannelId.clear();
	setState(ConnectionState::DISCONNECTED, "Logged out");
}

void DiscordClient::disconnect() {
	if (state.exchange(ConnectionState::DISCONNECTED) == ConnectionState::DISCONNECTED) {
		return;
	}

	Logger::log("DiscordClient::disconnect called");
	setStatus("Disconnected");

	// Joined without clientMutex, which the network thread takes to close the socket.
	if (networkThread.joinable()) {
		networkThread.join();
	}
	// Closing with 1000 ended the session, and the next connect may use another account.
	invalidateSession();

	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	isConnecting = false;

	{
		std::lock_guard<std::mutex> qLock(queueMutex);
		while (!messageQueue.empty()) {
			messageQueue.pop_front();
		}
	}

	{
		std::lock_guard<std::mutex> lock(sendQueueMutex);
		sendQueue.clear();
	}
}

void DiscordClient::queueSend(const std::string &message) {
	std::lock_guard<std::mutex> lock(sendQueueMutex);
	sendQueue.push_back(message);
}

void DiscordClient::runNetworkThread(const std::string &token) {
	Logger::log("[Network] Thread started");

	while (state != ConnectionState::DISCONNECTED) {
		connectionGeneration++;
		pendingFrames.clear();
		// Anything queued for the old connection would arrive before Identify.
		{
			std::lock_guard<std::mutex> lock(sendQueueMutex);
			sendQueue.clear();
		}
		// A new connection heartbeats only once its own Hello has been handled.
		heartbeatInterval = 0;
		waitingForHeartbeatAck = false;
		reconnectRequested = false;

		ws.setOnMessage([this](std::string &msg) { enqueueGatewayMessage(msg, false); });
		ws.setOnBinaryMessage([this](std::string &frame) { handleGatewayFrame(frame); });

		ws.setOnError([this](const std::string &err) {
			setStatus("Error: " + err);
			Logger::log("[Gateway] Error: %s", err.c_str());
		});

		ws.setOnClose([this](int code, const std::string &reason) {
			Logger::log("[Gateway] Closed: %d %s", code, reason.c_str());
			setStatus("Disconnected: " + std::to_string(code));
			if (code == 4004) {
				authFailed.store(true);
				setState(ConnectionState::DISCONNECTED, "Authentication failed");
			}
			// Discord will not resume a session closed with these.
			if (code == 4003 || code == 4007 || code == 4009) {
				invalidateSession();
			}
		});

		std::string gatewayUrl = DISCORD_GATEWAY_URL;
		bool resuming = false;
		{
			std::lock_guard<std::recursive_mutex> lock(clientMutex);
			if (!sessionId.empty() && lastSequence > 0 && !resumeGatewayUrl.empty()) {
				gatewayUrl = resumeGatewayUrl + DISCORD_GATEWAY_QUERY;
				resuming = true;
			}
		}

		setStatus(Core::I18n::getInstance().get("login.status.connecting"));
		if (!ws.connect(gatewayUrl)) {
			setStatus(Core::I18n::getInstance().get("login.status.connect_failed"));
			if (resuming) {
				Logger::log("[Gateway] Resume host unreachable, starting a new session");
				invalidateSession();
			} else {
				svcSleepThread(5ULL * 1000 * 1000 * 1000);
			}
			continue;
		}

		setStatus(Core::I18n::getInstance().get("login.status.waiting_hello"));
		{
			std::lock_guard<std::recursive_mutex> lock(clientMutex);
			isConnecting = false;
		}

		while (ws.isConnected() && state != ConnectionState::DISCONNECTED) {
			if (reconnectRequested.exchange(false)) {
				Logger::log("[Gateway] Reconnecting at the server's request");
				ws.disconnect(RESUMABLE_CLOSE_CODE);
				break;
			}

			const bool received = ws.poll();

			std::string msgToSend;
			bool hasMsg = false;
			{
				std::lock_guard<std::mutex> lock(sendQueueMutex);
				if (!sendQueue.empty()) {
					msgToSend = sendQueue.front();
					sendQueue.pop_front();
					hasMsg = true;
				}
			}

			if (hasMsg) {
				ws.send(msgToSend);
			}

			if (heartbeatInterval > 0) {
				uint64_t now = osGetTime();
				if (now - lastHeartbeat.load() >= (uint64_t)heartbeatInterval.load()) {
					if (waitingForHeartbeatAck) {
						Logger::log("[Gateway] Heartbeat ACK missing, reconnecting...");
						ws.disconnect(RESUMABLE_CLOSE_CODE);
						break;
					}
					sendHeartbeat();
					lastHeartbeat = now;
					waitingForHeartbeatAck = true;
				}
			}

			svcSleepThread(received || hasMsg ? Network::WebSocketClient::BUSY_POLL_NS
			                                  : Network::WebSocketClient::IDLE_POLL_NS);
		}

		if (state == ConnectionState::DISCONNECTED) {
			ws.disconnect();
			break;
		}

		bool hasSession;
		{
			std::lock_guard<std::recursive_mutex> lock(clientMutex);
			hasSession = !sessionId.empty();
		}

		if (hasSession) {
			Logger::log("[Gateway] Connection lost, attempting immediate reconnection...");
		} else {
			Logger::log("[Gateway] Login or critical error, retrying...");
		}

		setStatus(Core::I18n::getInstance().get("login.status.lost_connection"));
		ws.disconnect(RESUMABLE_CLOSE_CODE);
		if (!hasSession) {
			svcSleepThread(1ULL * 1000 * 1000 * 1000);
		}
	}

	Logger::log("[Network] Thread stopped");
}

void DiscordClient::workerLoop() {
	Logger::log("[Worker] Message processing thread started");
	while (true) {
		GatewayMessage message;
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueCv.wait(lock, [this] { return !messageQueue.empty() || stopWorker; });

			if (stopWorker && messageQueue.empty()) {
				break;
			}

			message = std::move(messageQueue.front());
			messageQueue.pop_front();
		}

		if (!message.data.empty()) {
			processGatewayMessage(message);
			Utils::requestRedraw();
		}
	}
	Logger::log("[Worker] Message processing thread stopped");
}

void DiscordClient::update() {
	time_t now = time(NULL);
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	for (auto it = typingUsers.begin(); it != typingUsers.end();) {
		auto &users = it->second;
		for (auto userIt = users.begin(); userIt != users.end();) {
			if (now - userIt->timestamp > 10) {
				userIt = users.erase(userIt);
				Utils::requestRedraw();
			} else {
				++userIt;
			}
		}
		if (users.empty()) {
			it = typingUsers.erase(it);
		} else {
			it++;
		}
	}
	sendNextReactionLocked();
}

void DiscordClient::triggerTypingIndicator(const std::string &channelId) {
	if (!Config::getInstance().isTypingIndicatorEnabled()) {
		return;
	}
	if (channelId.empty()) {
		return;
	}
	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/typing";
	Network::NetworkManager::getInstance().enqueue(url, "POST", "", Network::RequestPriority::INTERACTIVE,
	                                               [](const Network::HttpResponse &) {}, {{"Authorization", token}});
}

void DiscordClient::ringCall(const std::string &channelId) {
	if (channelId.empty()) {
		return;
	}
	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/call/ring";
	Network::NetworkManager::getInstance().enqueue(
	    url, "POST", "{}", Network::RequestPriority::INTERACTIVE,
	    [](const Network::HttpResponse &resp) { Logger::log("[Call] ring -> %d", resp.statusCode); },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

void DiscordClient::stopRinging(const std::string &channelId) {
	if (channelId.empty()) {
		return;
	}
	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		if (incomingCallChannelId == channelId) {
			incomingCallChannelId.clear();
		}
	}
	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/call/stop-ringing";
	Network::NetworkManager::getInstance().enqueue(
	    url, "POST", "{}", Network::RequestPriority::INTERACTIVE,
	    [](const Network::HttpResponse &resp) { Logger::log("[Call] stop-ringing -> %d", resp.statusCode); },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

std::string DiscordClient::getIncomingCallChannel() {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	return incomingCallChannelId;
}

std::optional<int> DiscordClient::getCallRingingCount(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	auto it = callRinging.find(channelId);
	if (it == callRinging.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::vector<TypingUser> DiscordClient::getTypingUsers(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (typingUsers.find(channelId) != typingUsers.end()) {
		return typingUsers[channelId];
	}
	return {};
}

void DiscordClient::addReaction(const std::string &channelId, const std::string &messageId, const Emoji &emoji) {
	queueReaction({channelId, messageId, emoji, true});
}

void DiscordClient::removeReaction(const std::string &channelId, const std::string &messageId, const Emoji &emoji) {
	queueReaction({channelId, messageId, emoji, false});
}

void DiscordClient::queueReaction(ReactionRequest request) {
	if (request.channelId.empty() || request.messageId.empty() ||
	    (request.emoji.id.empty() && request.emoji.name.empty())) {
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	notifyOwnReactionLocked(request, request.add);

	auto unsent = std::find_if(reactionQueue.begin() + (reactionInFlight ? 1 : 0), reactionQueue.end(),
	                           [&](const ReactionRequest &queued) {
		                           return queued.messageId == request.messageId && queued.emoji.matches(request.emoji);
	                           });
	if (unsent != reactionQueue.end()) {
		// Neither has reached Discord, so they cancel out.
		if (unsent->add != request.add) {
			reactionQueue.erase(unsent);
		}
		return;
	}
	reactionQueue.push_back(std::move(request));
	sendNextReactionLocked();
}

void DiscordClient::sendNextReactionLocked() {
	if (reactionInFlight || reactionQueue.empty() || osGetTime() < reactionRetryAt) {
		return;
	}
	reactionInFlight = true;

	const ReactionRequest &request = reactionQueue.front();
	const Emoji &emoji = request.emoji;
	// Only the delete route has the reaction type in its path; 0 is a normal reaction.
	std::string url = "https://discord.com/api/v10/channels/" + request.channelId + "/messages/" + request.messageId +
	                  "/reactions/" + urlEncode(emoji.id.empty() ? emoji.name : emoji.name + ":" + emoji.id) +
	                  (request.add ? "/@me" : "/0/@me");
	Network::NetworkManager::getInstance().enqueue(
	    url, request.add ? "PUT" : "DELETE", "", Network::RequestPriority::REALTIME,
	    [this](Network::HttpResponse &resp) { onReactionResponse(resp); }, {{"Authorization", token}});
}

void DiscordClient::onReactionResponse(Network::HttpResponse &resp) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	reactionInFlight = false;
	if (reactionQueue.empty()) {
		return;
	}

	std::optional<double> retryAfter = resp.statusCode == 429 ? retryAfterSeconds(resp) : std::nullopt;
	if (retryAfter) {
		// update() resends it once this passes.
		reactionRetryAt = osGetTime() + (uint64_t)(*retryAfter * 1000.0);
		return;
	}

	ReactionRequest request = std::move(reactionQueue.front());
	reactionQueue.pop_front();
	if (!resp.success) {
		Logger::log("[Discord] Failed to %s reaction: %ld %s", request.add ? "add" : "remove", resp.statusCode,
		            resp.body.c_str());
		notifyOwnReactionLocked(request, !request.add);
	}
	sendNextReactionLocked();
}

void DiscordClient::notifyOwnReactionLocked(const ReactionRequest &request, bool added) {
	const ReactionCallback &callback = added ? messageReactionAddCallback : messageReactionRemoveCallback;
	if (callback) {
		callback(request.channelId, request.messageId, currentUser.id, request.emoji);
	}
}

bool DiscordClient::hasQueuedReactionLocked(const std::string &messageId, const Emoji &emoji) {
	return std::any_of(reactionQueue.begin(), reactionQueue.end(), [&](const ReactionRequest &queued) {
		return queued.messageId == messageId && queued.emoji.matches(emoji);
	});
}

void DiscordClient::votePoll(const std::string &channelId, const std::string &messageId,
                             const std::vector<int> &answerIds) {
	if (channelId.empty() || messageId.empty()) {
		return;
	}

	std::string body = "{\"answer_ids\":[";
	for (size_t i = 0; i < answerIds.size(); i++) {
		if (i > 0) {
			body += ",";
		}
		body += std::to_string(answerIds[i]);
	}
	body += "]}";

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/polls/" + messageId + "/answers/@me";
	Network::NetworkManager::getInstance().enqueue(
	    url, "PUT", body, Network::RequestPriority::REALTIME,
	    [](const Network::HttpResponse &resp) {
		    if (!resp.success) {
			    Logger::log("[Discord] Failed to vote on poll: %ld %s", resp.statusCode, resp.body.c_str());
		    }
	    },
	    {{"Authorization", getInstance().token}, {"Content-Type", "application/json"}});
}

std::string DiscordClient::buildComponentInteraction(const Message &msg, const MessageControl &control,
                                                     const std::vector<std::string> &values, const std::string &nonce) {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("type");
	writer.Int(INTERACTION_MESSAGE_COMPONENT);
	writer.Key("nonce");
	writer.String(nonce.c_str());
	const std::string guildId = getGuildIdFromChannel(msg.channelId);
	if (!guildId.empty() && guildId != "DM") {
		writer.Key("guild_id");
		writer.String(guildId.c_str());
	}
	writer.Key("channel_id");
	writer.String(msg.channelId.c_str());
	writer.Key("message_id");
	writer.String(msg.id.c_str());
	writer.Key("message_flags");
	writer.Int(msg.flags);
	// Only responses and OAuth2 messages carry application_id; a bot's own messages come from its user.
	writer.Key("application_id");
	writer.String((msg.applicationId.empty() ? msg.author.id : msg.applicationId).c_str());
	writer.Key("session_id");
	writer.String(getSessionId().c_str());
	writer.Key("data");
	writer.StartObject();
	writer.Key("component_type");
	writer.Int(control.type);
	writer.Key("custom_id");
	writer.String(control.customId.c_str());
	if (!control.isButton()) {
		writer.Key("values");
		writer.StartArray();
		for (const auto &value : values) {
			writer.String(value.c_str());
		}
		writer.EndArray();
	}
	writer.EndObject();
	writer.EndObject();
	return s.GetString();
}

void DiscordClient::sendComponentInteraction(const Message &msg, const MessageControl &control,
                                             const std::vector<std::string> &values, const std::string &nonce,
                                             SuccessCallback cb) {
	if (token.empty()) {
		cb(false);
		return;
	}

	Network::NetworkManager::getInstance().enqueue(
	    "https://discord.com/api/v10/interactions", "POST", buildComponentInteraction(msg, control, values, nonce),
	    Network::RequestPriority::REALTIME,
	    [cb](const Network::HttpResponse &resp) {
		    if (!resp.success) {
			    Logger::log("[Discord] Failed to create interaction: %ld %s", resp.statusCode, resp.body.c_str());
		    }
		    cb(resp.success);
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

std::string DiscordClient::buildModalSubmit(const Modal &modal, const std::string &nonce) {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("type");
	writer.Int(INTERACTION_MODAL_SUBMIT);
	writer.Key("nonce");
	writer.String(nonce.c_str());
	const std::string guildId = getGuildIdFromChannel(modal.channelId);
	if (!guildId.empty() && guildId != "DM") {
		writer.Key("guild_id");
		writer.String(guildId.c_str());
	}
	writer.Key("channel_id");
	writer.String(modal.channelId.c_str());
	writer.Key("application_id");
	writer.String(modal.applicationId.c_str());
	writer.Key("session_id");
	writer.String(getSessionId().c_str());
	writer.Key("data");
	writer.StartObject();
	writer.Key("id");
	writer.String(modal.id.c_str());
	writer.Key("custom_id");
	writer.String(modal.customId.c_str());
	writer.Key("components");
	writer.StartArray();
	for (const auto &field : modal.fields) {
		if (field.wrapperType != ComponentType::ACTION_ROW && field.wrapperType != ComponentType::LABEL) {
			continue;
		}
		writer.StartObject();
		writer.Key("type");
		writer.Int(field.wrapperType);
		if (field.wrapperType == ComponentType::ACTION_ROW) {
			writer.Key("components");
			writer.StartArray();
		} else {
			writer.Key("component");
		}

		writer.StartObject();
		writer.Key("type");
		writer.Int(field.control.type);
		writer.Key("custom_id");
		writer.String(field.control.customId.c_str());
		if (field.control.type == ComponentType::TEXT_INPUT) {
			writer.Key("value");
			writer.String(field.value.c_str());
		} else if (field.control.type == ComponentType::CHECKBOX) {
			writer.Key("value");
			writer.Bool(field.checked);
		} else if (field.control.type == ComponentType::RADIO_GROUP) {
			writer.Key("value");
			if (field.control.chosen.empty()) {
				writer.Null();
			} else {
				writer.String(field.control.chosen[0].c_str());
			}
		} else {
			writer.Key("values");
			writer.StartArray();
			for (const auto &val : field.control.chosen) {
				writer.String(val.c_str());
			}
			writer.EndArray();
		}
		writer.EndObject();

		if (field.wrapperType == ComponentType::ACTION_ROW) {
			writer.EndArray();
		}
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
	writer.EndObject();
	return s.GetString();
}

void DiscordClient::sendModalSubmit(const Modal &modal, const std::string &nonce, SuccessCallback cb) {
	if (token.empty()) {
		cb(false);
		return;
	}

	Network::NetworkManager::getInstance().enqueue("https://discord.com/api/v10/interactions", "POST",
	                                               buildModalSubmit(modal, nonce), Network::RequestPriority::REALTIME,
	                                               [cb](const Network::HttpResponse &resp) {
		                                               if (!resp.success) {
			                                               Logger::log("[Discord] Failed to submit modal: %ld %s",
			                                                           resp.statusCode, resp.body.c_str());
		                                               }
		                                               cb(resp.success);
	                                               },
	                                               {{"Authorization", token}, {"Content-Type", "application/json"}});
}

void DiscordClient::setState(ConnectionState newState, const std::string &message) {
	state = newState;
	setStatus(message);
	Logger::log("[Gateway] State: %d, Msg: %s", (int)newState, message.c_str());
}

void DiscordClient::setStatus(const std::string &message) {
	{
		std::lock_guard<std::mutex> lock(statusMutex);
		statusMessage = message;
	}
	Utils::requestRedraw();
}

void DiscordClient::enqueueGatewayMessage(std::string &data, bool compressed) {
	if (data.empty()) {
		return;
	}

	GatewayMessage message;
	message.data = std::move(data);
	message.compressed = compressed;
	message.generation = connectionGeneration;
	{
		std::lock_guard<std::mutex> lock(queueMutex);
		messageQueue.push_back(std::move(message));
	}
	queueCv.notify_one();
}

void DiscordClient::handleGatewayFrame(std::string &frame) {
	if (pendingFrames.empty()) {
		pendingFrames.swap(frame);
	} else {
		pendingFrames.append(frame);
	}

	static const char SYNC_FLUSH[4] = {'\x00', '\x00', '\xFF', '\xFF'};
	if (pendingFrames.size() < sizeof(SYNC_FLUSH) ||
	    memcmp(pendingFrames.data() + pendingFrames.size() - sizeof(SYNC_FLUSH), SYNC_FLUSH, sizeof(SYNC_FLUSH)) != 0) {
		return;
	}

	enqueueGatewayMessage(pendingFrames, true);
	pendingFrames.clear();
}

void DiscordClient::processGatewayMessage(GatewayMessage &message) {
	std::string json;
	std::string event;

	if (message.compressed) {
		if (message.generation != inflaterGeneration) {
			if (!inflater.reset()) {
				Logger::log("[Gateway] inflateInit failed");
			}
			inflaterGeneration = message.generation;
		}
		inflater.setInput(message.data);

		Network::JsonInputStream stream(inflater);
		// Discord sends "t" ahead of "d", so it sits in the first inflated chunk.
		if (peekEventName(stream.pending(), stream.pendingSize(), event)) {
			if (isStreamedEvent(event)) {
				processStreamedMessage(stream, event);
				// The inflate context has to consume every message, parsed or not.
				stream.drain();
				return;
			}
			json = stream.readAll();
			processMessage(json);
			return;
		}
		json = stream.readAll();
	} else {
		json = std::move(message.data);
	}

	if (json.empty()) {
		return;
	}
	if (peekEventName(json.data(), json.size(), event) && isStreamedEvent(event)) {
		Network::JsonInputStream stream(json.data(), json.size());
		processStreamedMessage(stream, event);
		return;
	}
	processMessage(json);
}

void DiscordClient::processStreamedMessage(Network::JsonInputStream &stream, const std::string &event) {
	using Token = JsonPull::Token;
	JsonPull p(stream);
	std::unique_ptr<ReadyData> ready;
	std::unique_ptr<Guild> guild;
	rapidjson::Document voiceStates;
	voiceStates.SetArray();
	bool unavailable = false;
	bool hasSequence = false;
	uint64_t sequence = 0;

	bool parsed = p.next() && p.token() == Token::START_OBJECT;
	std::string key;
	while (parsed && p.nextKey(key)) {
		if (key == "s") {
			parsed = p.readValue();
			hasSequence = p.token() == Token::NUMBER;
			sequence = p.asUint64();
		} else if (key != "d") {
			parsed = p.skipValue();
		} else if (event == "READY") {
			ready = std::make_unique<ReadyData>();
			parsed = parseReady(p, *ready, voiceStates);
		} else if (event == "GUILD_CREATE") {
			guild = std::make_unique<Guild>();
			std::string userId = getCurrentUser().id;
			parsed = p.next() && p.token() == Token::START_OBJECT &&
			         parseGuildFields(p, *guild, userId, &voiceStates, hasRoomForGuildData(), &unavailable);
		} else if (event == "READY_SUPPLEMENTAL") {
			parsed = parseReadySupplemental(p, voiceStates);
		} else {
			parsed = p.skipValue();
		}
	}

	if (!parsed || !p.ok()) {
		Logger::log("[Gateway] Failed to parse %s (error %d at %u)", event.c_str(), p.errorCode(),
		            (unsigned)p.errorOffset());
		return;
	}

	if (hasSequence) {
		lastSequence = sequence;
	}
	if (ready) {
		applyReady(*ready, voiceStates);
	} else if (guild) {
		// An outage sends the guild as unavailable, which must not wipe what is cached.
		if (!unavailable) {
			applyGuildCreate(*guild, voiceStates);
		}
	} else if (!voiceStates.Empty()) {
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		applyVoiceStatesLocked(voiceStates);
	}
}

void DiscordClient::processMessage(std::string &message) {
	rapidjson::Document doc;

	doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(&message[0]);

	if (doc.HasParseError()) {
		Logger::log("JSON parse error: %s offset %u", rapidjson::GetParseError_En(doc.GetParseError()),
		            (unsigned)doc.GetErrorOffset());
		return;
	}

	if (!doc.IsObject()) {
		return;
	}

	int op = Utils::Json::getInt(doc, "op", -1);
	// "s" is null outside dispatches.
	auto seq = doc.FindMember("s");
	if (seq != doc.MemberEnd() && seq->value.IsUint64()) {
		lastSequence = seq->value.GetUint64();
	}

	std::string t = Utils::Json::getString(doc, "t");

	switch (op) {
	case 7: // Reconnect
		handleReconnect();
		break;

	case 9: // Invalid Session
		handleInvalidSession(doc);
		break;

	case 10: // Hello
		handleHello(doc);
		break;

	case 11: // Heartbeat ACK
		waitingForHeartbeatAck = false;
		break;

	case 0: // Dispatch
		handleDispatch(doc);
		break;

	default:
		break;
	}
}

void DiscordClient::handleHello(const rapidjson::Document &doc) {
	if (doc.HasMember("d") && doc["d"].IsObject()) {
		const rapidjson::Value &d = doc["d"];
		int interval = (int)Utils::Json::getUint64(d, "heartbeat_interval");
		if (interval > 0) {
			Logger::log("[Gateway] Hello received. Heartbeat interval: %d ms", interval);

			lastHeartbeat = osGetTime();
			waitingForHeartbeatAck = false;
			heartbeatInterval = interval;
			sendHeartbeat();
			setStatus(Core::I18n::getInstance().get("login.status.authenticating"));

			bool resume;
			{
				std::lock_guard<std::recursive_mutex> lock(clientMutex);
				resume = !sessionId.empty() && lastSequence > 0;
			}
			if (resume) {
				sendResume();
			} else {
				sendIdentify();
			}
		}
	}
}

void DiscordClient::handleDispatch(const rapidjson::Document &doc) {
	std::string t = Utils::Json::getString(doc, "t");

	if (t != "READY" && t != "GUILD_CREATE" && t != "PRESENCE_UPDATE") {
		Logger::log("[Gateway] Dispatch: %s", t.c_str());
	}

	if (t == "RESUMED") {
		handleResumed();
		return;
	}

	if (!doc.HasMember("d") || !doc["d"].IsObject()) {
		return;
	}
	const rapidjson::Value &d = doc["d"];

	// An unloaded guild has no channel list to search for this mapping later.
	std::string eventChannel = Utils::Json::getString(d, "channel_id");
	std::string eventGuild = Utils::Json::getString(d, "guild_id");
	if (!eventChannel.empty() && !eventGuild.empty()) {
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		channelToGuildCache[eventChannel] = eventGuild;
	}

	if (t == "VOICE_STATE_UPDATE") {
		applyVoiceState(d, Utils::Json::getString(d, "guild_id"));
		guildDataDirty = true;
		if (Utils::Json::getString(d, "user_id") == currentUser.id) {
			std::string voiceSession = Utils::Json::getString(d, "session_id");
			bool serverMute = Utils::Json::getBool(d, "mute");
			bool serverDeaf = Utils::Json::getBool(d, "deaf");
			Logger::log("[Voice] VOICE_STATE_UPDATE session=%s mute=%d deaf=%d", voiceSession.c_str(), serverMute,
			            serverDeaf);
			if (voiceStateCallback) {
				voiceStateCallback(voiceSession, serverMute, serverDeaf);
			}
		}
		return;
	}

	if (t == "VOICE_SERVER_UPDATE") {
		std::string token = Utils::Json::getString(d, "token");
		std::string endpoint = Utils::Json::getString(d, "endpoint");
		std::string serverId = Utils::Json::getString(d, "guild_id");
		if (serverId.empty()) {
			serverId = Utils::Json::getString(d, "channel_id");
		}
		Logger::log("[Voice] VOICE_SERVER_UPDATE endpoint=%s server=%s", endpoint.c_str(), serverId.c_str());
		if (voiceServerCallback) {
			voiceServerCallback(token, endpoint, serverId);
		}
		return;
	}

	if (t == "GUILD_MEMBERS_CHUNK") {
		handleGuildMembersChunk(d);
		return;
	}

	if (t == "CALL_CREATE" || t == "CALL_UPDATE") {
		std::string channelId = Utils::Json::getString(d, "channel_id");
		std::string self = getCurrentUser().id;
		bool ringingUs = false;
		int ringingCount = 0;
		if (d.HasMember("ringing") && d["ringing"].IsArray()) {
			for (const auto &r : d["ringing"].GetArray()) {
				if (!r.IsString()) {
					continue;
				}
				ringingCount++;
				if (self == r.GetString()) {
					ringingUs = true;
				}
			}
		}

		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		callRinging[channelId] = ringingCount;
		if (ringingUs) {
			if (incomingCallChannelId != channelId) {
				Logger::log("[Call] Incoming call in %s", channelId.c_str());
			}
			incomingCallChannelId = channelId;
		} else if (incomingCallChannelId == channelId) {
			incomingCallChannelId.clear();
		}

		if (d.HasMember("voice_states") && d["voice_states"].IsArray()) {
			for (const auto &vs : d["voice_states"].GetArray()) {
				if (vs.IsObject()) {
					applyVoiceState(vs, "");
				}
			}
		}
		return;
	}

	if (t == "CALL_DELETE") {
		std::string channelId = Utils::Json::getString(d, "channel_id");
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		callRinging[channelId] = 0;
		if (incomingCallChannelId == channelId) {
			Logger::log("[Call] Call in %s ended", channelId.c_str());
			incomingCallChannelId.clear();
		}
		return;
	}

	if (t == "CHANNEL_CREATE" || t == "CHANNEL_UPDATE") {
		handleChannelCreateUpdate(d);
	} else if (t == "CHANNEL_DELETE") {
		handleChannelDelete(d);
	} else if (t == "TYPING_START") {
		handleTypingStart(d);
	} else if (t == "MESSAGE_CREATE") {
		handleMessageCreate(d);
	} else if (t == "MESSAGE_UPDATE") {
		handleMessageUpdate(d);
	} else if (t == "MESSAGE_DELETE") {
		handleMessageDelete(d);
	} else if (t == "MESSAGE_REACTION_ADD") {
		handleReactionAdd(d);
	} else if (t == "MESSAGE_REACTION_REMOVE") {
		handleReactionRemove(d);
	} else if (t == "MESSAGE_POLL_VOTE_ADD") {
		handlePollVote(d, true);
	} else if (t == "MESSAGE_POLL_VOTE_REMOVE") {
		handlePollVote(d, false);
	} else if (t == "INTERACTION_SUCCESS") {
		handleInteractionResult(d, InteractionResult::SUCCESS);
	} else if (t == "INTERACTION_FAILURE") {
		handleInteractionResult(d, InteractionResult::FAILURE);
	} else if (t == "INTERACTION_MODAL_CREATE") {
		handleModalCreate(d);
	} else if (t == "INTERACTION_IFRAME_MODAL_CREATE") {
		handleInteractionResult(d, InteractionResult::MODAL);
	} else if (t == "PRESENCE_UPDATE") {
		handlePresenceUpdate(d);
	} else if (t == "USER_SETTINGS_UPDATE") {
		handleUserSettingsUpdate(d);
	} else if (t == "MESSAGE_ACK") {
		handleMessageAck(d);
	} else if (t == "USER_GUILD_SETTINGS_UPDATE") {
		handleUserGuildSettingsUpdate(d);
	} else if (t == "SESSIONS_REPLACE") {
		handleSessionsReplace(d);
	} else if (t == "THREAD_CREATE" || t == "THREAD_UPDATE") {
		handleChannelCreateUpdate(d);
	} else if (t == "THREAD_LIST_SYNC") {
		if (d.HasMember("threads") && d["threads"].IsArray()) {
			std::string syncGuildId = Utils::Json::getString(d, "guild_id");
			const rapidjson::Value &threads = d["threads"];
			for (rapidjson::SizeType i = 0; i < threads.Size(); i++) {
				handleChannelCreateUpdate(threads[i], syncGuildId);
			}
		}
	}
}

namespace {

// NONE means the stream failed or the container ended where a value was expected.
JsonPull::Token openValue(JsonPull &p) {
	if (!p.next() || p.token() == JsonPull::Token::END_OBJECT || p.token() == JsonPull::Token::END_ARRAY) {
		return JsonPull::Token::NONE;
	}
	return p.token();
}

// Walks the members of an object whose opening brace was just consumed.
template <typename Fn> bool forEachMember(JsonPull &p, Fn fn) {
	std::string key;
	while (p.nextKey(key)) {
		if (!fn(key)) {
			return false;
		}
	}
	return p.ok();
}

template <typename Fn> bool captureObjects(JsonPull &p, rapidjson::Document &item, Fn fn) {
	while (p.captureValue(item)) {
		if (item.IsObject()) {
			fn(item);
		}
	}
	return p.ok();
}

// Calls fn with each object of the array value that comes next; any other value is skipped.
template <typename Fn> bool forEachElement(JsonPull &p, rapidjson::Document &item, Fn fn) {
	switch (openValue(p)) {
	case JsonPull::Token::START_ARRAY:
		return captureObjects(p, item, fn);
	case JsonPull::Token::START_OBJECT:
		return p.skipRest();
	case JsonPull::Token::NONE:
		return false;
	default:
		return true;
	}
}

// Versioned READY fields come either as an array or as {"entries": [...]}.
template <typename Fn> bool forEachEntry(JsonPull &p, rapidjson::Document &item, Fn fn) {
	switch (openValue(p)) {
	case JsonPull::Token::START_ARRAY:
		return captureObjects(p, item, fn);
	case JsonPull::Token::START_OBJECT:
		return forEachMember(
		    p, [&](const std::string &key) { return key == "entries" ? forEachElement(p, item, fn) : p.skipValue(); });
	case JsonPull::Token::NONE:
		return false;
	default:
		return true;
	}
}

// body() starts after the object's '{' and must consume through its '}'.
template <typename Body> bool forEachObject(JsonPull &p, Body body) {
	switch (openValue(p)) {
	case JsonPull::Token::START_ARRAY:
		break;
	case JsonPull::Token::START_OBJECT:
		return p.skipRest();
	case JsonPull::Token::NONE:
		return false;
	default:
		return true;
	}

	while (p.next()) {
		if (p.token() == JsonPull::Token::END_ARRAY) {
			return true;
		}
		bool good = true;
		if (p.token() == JsonPull::Token::START_OBJECT) {
			good = body();
		} else if (p.token() == JsonPull::Token::START_ARRAY) {
			good = p.skipRest();
		}
		if (!good) {
			return false;
		}
	}
	return false;
}

Role parseRoleObject(const rapidjson::Value &roleObj) {
	Role role;
	role.id = Utils::Json::getString(roleObj, "id");
	role.name = Utils::Json::getString(roleObj, "name");
	role.color = Utils::Json::getInt(roleObj, "color");
	role.position = Utils::Json::getInt(roleObj, "position");
	role.permissions = Utils::Json::getUint64(roleObj, "permissions");
	return role;
}

GuildFolder parseGuildFolder(const rapidjson::Value &folderObj) {
	GuildFolder folder;
	folder.id = Utils::Json::getString(folderObj, "id");
	folder.name = Utils::Json::getString(folderObj, "name");
	folder.color = Utils::Json::getInt(folderObj, "color");

	if (folderObj.HasMember("guild_ids") && folderObj["guild_ids"].IsArray()) {
		for (const auto &id : folderObj["guild_ids"].GetArray()) {
			if (id.IsString()) {
				folder.guildIds.push_back(id.GetString());
			}
		}
	}
	return folder;
}

void releaseGuildDetails(Guild &guild) {
	std::vector<Channel>().swap(guild.channels);
	std::vector<Role>().swap(guild.roles);
	std::vector<std::string>().swap(guild.myRoles);
	guild.channelsLoaded = false;
	guild.loadedOnDemand = false;
}

} // namespace

struct DiscordClient::ReadyData {
	std::string sessionId;
	User user;
	std::vector<std::pair<std::string, std::string>> sessionStatuses;
	std::vector<Guild> guilds;
	std::vector<Channel> privateChannels;
	std::vector<GuildFolder> folders;
	std::map<std::string, GuildNotificationSettings> notificationSettings;
	std::map<std::string, ReadState> readStates;
	std::string resumeGatewayUrl;
};

Guild *DiscordClient::findGuildLocked(const std::string &guildId) {
	for (auto &guild : guilds) {
		if (guild.id == guildId) {
			return &guild;
		}
	}
	return nullptr;
}

bool DiscordClient::parseReady(JsonPull &p, ReadyData &ready, rapidjson::Document &voiceStates) {
	if (!p.next() || p.token() != JsonPull::Token::START_OBJECT) {
		return false;
	}

	Utils::Json::ScratchDocument scratch;
	rapidjson::Document &item = scratch.get();
	bool keepPrivateChannels = true;
	bool keepReadStates = true;

	return forEachMember(p, [&](const std::string &key) {
		if (key == "session_id") {
			return p.readString(ready.sessionId);
		}
		if (key == "resume_gateway_url") {
			return p.readString(ready.resumeGatewayUrl);
		}
		if (key == "user") {
			if (!p.captureValue(item)) {
				return false;
			}
			if (item.IsObject()) {
				ready.user = parseUserObject(item);
			}
			return true;
		}
		if (key == "sessions") {
			return forEachElement(p, item, [&](const rapidjson::Value &s) {
				ready.sessionStatuses.emplace_back(Utils::Json::getString(s, "session_id"),
				                                   Utils::Json::getString(s, "status"));
			});
		}
		if (key == "guilds") {
			return parseReadyGuilds(p, ready, voiceStates);
		}
		if (key == "private_channels") {
			setStatus(Core::I18n::getInstance().get("login.status.loading_direct_messages"));
			bool good = parseChannelArray(p, item, ready.privateChannels, keepPrivateChannels);
			if (!keepPrivateChannels) {
				Logger::log("[Gateway] Low memory: keeping %zu private channels", ready.privateChannels.size());
			}
			return good;
		}
		if (key == "user_settings") {
			setStatus(Core::I18n::getInstance().get("login.status.processing_settings"));
			JsonPull::Token opened = openValue(p);
			if (opened != JsonPull::Token::START_OBJECT) {
				return opened == JsonPull::Token::START_ARRAY ? p.skipRest() : opened != JsonPull::Token::NONE;
			}
			return forEachMember(p, [&](const std::string &settingKey) {
				if (settingKey != "guild_folders") {
					return p.skipValue();
				}
				return forEachElement(p, item,
				                      [&](const rapidjson::Value &f) { ready.folders.push_back(parseGuildFolder(f)); });
			});
		}
		if (key == "user_guild_settings") {
			return forEachEntry(p, item, [&](const rapidjson::Value &entry) {
				parseUserGuildSettings(entry, ready.notificationSettings);
			});
		}
		if (key == "read_state") {
			return forEachEntry(p, item, [&](const rapidjson::Value &entry) {
				if (!keepReadStates) {
					return;
				}
				if (ready.readStates.size() % 256 == 255 && !hasRoomForGuildData()) {
					keepReadStates = false;
					Logger::log("[Gateway] Low memory: keeping %zu read states", ready.readStates.size());
					return;
				}
				ReadState rs;
				rs.channelId = Utils::Json::getString(entry, "id");
				rs.lastReadMessageId = Utils::Json::getString(entry, "last_message_id");
				rs.mentionCount = Utils::Json::getInt(entry, "mention_count");
				if (!rs.channelId.empty()) {
					ready.readStates[rs.channelId] = std::move(rs);
				}
			});
		}
		return p.skipValue();
	});
}

bool DiscordClient::parseReadyGuilds(JsonPull &p, ReadyData &ready, rapidjson::Document &voiceStates) {
	const std::string loadingText = Core::I18n::getInstance().get("login.status.loading_guilds");
	bool keepDetails = true;
	return forEachObject(p, [&]() {
		const int loading = (int)ready.guilds.size() + 1;
		const int total = expectedGuildCount.load();
		std::string progress = std::to_string(loading);
		if (total > 0) {
			progress += "/" + std::to_string(std::max(total, loading));
		}
		setStatus(loadingText + " (" + progress + ")...");
		const bool wasKeeping = keepDetails;
		keepDetails = keepDetails && hasRoomForGuildData();

		Guild guild;
		if (!parseGuildFields(p, guild, ready.user.id, &voiceStates, keepDetails)) {
			return false;
		}
		keepDetails = keepDetails && guild.channelsLoaded;
		if (wasKeeping && !keepDetails) {
			Logger::log("[Gateway] Low memory after %zu guilds; the rest load on demand", ready.guilds.size());
		}
		ready.guilds.push_back(std::move(guild));
		return true;
	});
}

// With PRIORITIZED_READY_PAYLOAD, voice states arrive here instead of in READY.
bool DiscordClient::parseReadySupplemental(JsonPull &p, rapidjson::Document &voiceStates) {
	if (!p.next() || p.token() != JsonPull::Token::START_OBJECT) {
		return false;
	}

	return forEachMember(p, [&](const std::string &key) {
		if (key != "guilds") {
			return p.skipValue();
		}
		return forEachObject(p, [&]() {
			Guild guild;
			return parseGuildFields(p, guild, "", &voiceStates, false);
		});
	});
}

void DiscordClient::applyVoiceStatesLocked(rapidjson::Document &voiceStates) {
	for (const auto &state : voiceStates.GetArray()) {
		applyVoiceState(state, Utils::Json::getString(state, "guild_id"));
	}
	if (!voiceStates.Empty()) {
		guildDataDirty.store(true);
	}
}

void DiscordClient::invalidateSession() {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	sessionId.clear();
	resumeGatewayUrl.clear();
	lastSequence = 0;
}

// Expects the opening brace consumed; without keepDetails the guild is left to load on demand.
bool DiscordClient::parseGuildFields(JsonPull &p, Guild &guild, const std::string &userId,
                                     rapidjson::Document *voiceStates, bool keepDetails, bool *unavailable) {
	Utils::Json::ScratchDocument scratch;
	rapidjson::Document &item = scratch.get();
	rapidjson::SizeType firstVoiceState = voiceStates ? voiceStates->Size() : 0;
	int memberCount = 0;

	bool good = forEachMember(p, [&](const std::string &key) {
		if (key == "id") {
			return p.readString(guild.id);
		}
		if (key == "name") {
			return p.readString(guild.name);
		}
		if (key == "icon") {
			return p.readString(guild.icon);
		}
		if (key == "owner_id") {
			return p.readString(guild.ownerId);
		}
		if (key == "rules_channel_id") {
			return p.readString(guild.rules_channel_id);
		}
		if (key == "description") {
			return p.readString(guild.description);
		}
		if (key == "approximate_member_count") {
			bool read = p.readValue();
			guild.approximateMemberCount = (int)p.asInt64();
			return read;
		}
		if (key == "approximate_presence_count") {
			bool read = p.readValue();
			guild.approximatePresenceCount = (int)p.asInt64();
			return read;
		}
		if (key == "member_count") {
			bool read = p.readValue();
			memberCount = (int)p.asInt64();
			return read;
		}
		if (key == "unavailable") {
			bool read = p.readValue();
			if (unavailable) {
				*unavailable = p.asBool();
			}
			return read;
		}
		if (key == "properties") {
			// CLIENT_STATE_V2 moves the plain guild fields into this object.
			JsonPull::Token opened = openValue(p);
			if (opened != JsonPull::Token::START_OBJECT) {
				return opened == JsonPull::Token::START_ARRAY ? p.skipRest() : opened != JsonPull::Token::NONE;
			}
			return parseGuildFields(p, guild, userId, nullptr, keepDetails);
		}
		if (key == "roles" && keepDetails) {
			guild.roles.clear();
			return forEachElement(p, item,
			                      [&](const rapidjson::Value &r) { guild.roles.push_back(parseRoleObject(r)); });
		}
		if (key == "members" && keepDetails) {
			return forEachElement(p, item, [&](const rapidjson::Value &m) {
				std::string memberId;
				auto user = m.FindMember("user");
				if (user != m.MemberEnd() && user->value.IsObject()) {
					memberId = Utils::Json::getString(user->value, "id");
				} else {
					memberId = Utils::Json::getString(m, "user_id");
				}
				if (memberId.empty()) {
					return;
				}
				// READY may list guilds before the user, so keep candidates until it is known.
				if (userId.empty()) {
					guild.members.push_back(parseMemberObject(m, memberId));
				} else if (memberId == userId) {
					guild.myRoles = parseMemberObject(m, memberId).role_ids;
				}
			});
		}
		if (key == "channels" && keepDetails) {
			guild.channels.clear();
			return parseChannelArray(p, item, guild.channels, keepDetails);
		}
		if (key == "voice_states" && voiceStates) {
			return forEachElement(p, item, [&](const rapidjson::Value &state) {
				rapidjson::Value copy(state, voiceStates->GetAllocator());
				voiceStates->PushBack(copy, voiceStates->GetAllocator());
			});
		}
		return p.skipValue();
	});
	if (!good) {
		return false;
	}

	if (guild.approximateMemberCount == 0) {
		guild.approximateMemberCount = memberCount;
	}

	if (voiceStates) {
		rapidjson::Document::AllocatorType &alloc = voiceStates->GetAllocator();
		for (rapidjson::SizeType i = firstVoiceState; i < voiceStates->Size(); i++) {
			rapidjson::Value &state = (*voiceStates)[i];
			if (!state.HasMember("guild_id")) {
				rapidjson::Value guildId(guild.id.c_str(), alloc);
				state.AddMember("guild_id", guildId, alloc);
			}
		}
	}

	if (keepDetails) {
		guild.channels.shrink_to_fit();
		guild.roles.shrink_to_fit();
	} else {
		std::vector<Member>().swap(guild.members);
		releaseGuildDetails(guild);
	}
	return true;
}

// Stops appending once the heap runs low, and clears fits.
bool DiscordClient::parseChannelArray(JsonPull &p, rapidjson::Document &item, std::vector<Channel> &out, bool &fits) {
	return forEachElement(p, item, [&](const rapidjson::Value &c) {
		if (!fits) {
			return;
		}
		if (out.size() % 64 == 63 && !hasRoomForGuildData()) {
			fits = false;
			return;
		}
		Channel channel;
		parseChannelObject(c, channel);
		out.push_back(std::move(channel));
	});
}

void DiscordClient::finishParsedGuild(Guild &guild, const std::string &userId) {
	if (guild.myRoles.empty()) {
		for (auto &member : guild.members) {
			if (member.user_id == userId) {
				guild.myRoles = std::move(member.role_ids);
				break;
			}
		}
	}
	std::vector<Member>().swap(guild.members);
}

void DiscordClient::computeViewable(Guild &guild, const std::string &userId) {
	uint64_t base = calcBasePermissions(guild, userId, guild.myRoles);
	if (base & Permissions::ADMINISTRATOR) {
		for (auto &channel : guild.channels) {
			channel.viewable = true;
		}
		return;
	}

	std::unordered_map<std::string_view, const Channel *> byId;
	byId.reserve(guild.channels.size());
	for (const auto &channel : guild.channels) {
		byId.emplace(channel.id, &channel);
	}

	for (auto &channel : guild.channels) {
		const Channel *parent = nullptr;
		if (!channel.parent_id.empty()) {
			auto found = byId.find(channel.parent_id);
			if (found != byId.end()) {
				parent = found->second;
			}
		}
		uint64_t perms = applyChannelOverwrites(base, guild, parent, channel, userId, guild.myRoles);
		channel.viewable = (perms & Permissions::VIEW_CHANNEL) != 0;
	}
}

void DiscordClient::applyReady(ReadyData &ready, rapidjson::Document &voiceStates) {
	if (!ready.sessionId.empty()) {
		Logger::log("[Gateway] READY: Session ID = %s", ready.sessionId.c_str());
	}
	for (const auto &session : ready.sessionStatuses) {
		if (session.first == ready.sessionId) {
			ready.user.status = stringToStatus(session.second);
			break;
		}
	}

	if (connectionCallback) {
		connectionCallback();
	}

	Logger::log("[Gateway] Parsed %zu guilds, %zu private channels, %zu notification settings, %zu read states",
	            ready.guilds.size(), ready.privateChannels.size(), ready.notificationSettings.size(),
	            ready.readStates.size());

	setStatus(Core::I18n::getInstance().get("login.status.processing_settings"));
	for (auto &guild : ready.guilds) {
		finishParsedGuild(guild, ready.user.id);
		if (guild.channelsLoaded) {
			computeViewable(guild, ready.user.id);
		}
	}

	std::unordered_map<std::string_view, size_t> folderRank;
	for (const auto &folder : ready.folders) {
		for (const auto &id : folder.guildIds) {
			folderRank.emplace(id, folderRank.size());
		}
	}
	if (!folderRank.empty()) {
		setStatus("Sorting guilds...");
		// Guilds outside every folder keep their READY order after the rest.
		auto rank = [&](const Guild &guild) {
			auto found = folderRank.find(guild.id);
			return found != folderRank.end() ? found->second : SIZE_MAX;
		};
		std::stable_sort(ready.guilds.begin(), ready.guilds.end(),
		                 [&](const Guild &a, const Guild &b) { return rank(a) < rank(b); });
	}

	setStatus("Finalizing login...");
	Logger::log("[Gateway] Locking clientMutex to finalize READY...");
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	channelToGuildCache.clear();
	sessionId = ready.sessionId;
	resumeGatewayUrl = ready.resumeGatewayUrl;
	while (!resumeGatewayUrl.empty() && resumeGatewayUrl.back() == '/') {
		resumeGatewayUrl.pop_back();
	}
	currentUser = ready.user;
	guilds = std::move(ready.guilds);
	privateChannels = std::move(ready.privateChannels);
	folders = std::move(ready.folders);
	readStates = std::move(ready.readStates);
	notificationSettings = std::move(ready.notificationSettings);
	guildLoadFailed.clear();

	voiceParticipants.clear();
	voiceChannelByUser.clear();
	applyVoiceStatesLocked(voiceStates);

	Config::getInstance().updateCurrentAccountName(currentUser.username);

	setState(ConnectionState::READY, "Ready! Logged in as " + currentUser.username);
}

void DiscordClient::applyGuildCreate(Guild &guild, rapidjson::Document &voiceStates) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	finishParsedGuild(guild, currentUser.id);
	const std::string guildId = guild.id;

	Guild *existing = findGuildLocked(guildId);
	if (existing) {
		applyGuildDetailsLocked(*existing, guild);
		// A payload that did not fit must leave the cached channels in place.
		if (existing->channelsLoaded && guild.channelsLoaded) {
			if (!guild.myRoles.empty()) {
				existing->myRoles = std::move(guild.myRoles);
			}
			existing->channels = std::move(guild.channels);
			computeViewable(*existing, currentUser.id);
		}
		Logger::log("Updated existing guild %s (merged)", existing->name.c_str());
	} else {
		if (guild.channelsLoaded) {
			computeViewable(guild, currentUser.id);
		}
		guilds.push_back(std::move(guild));
		Logger::log("Added new guild %s", guilds.back().name.c_str());
	}

	applyVoiceStatesLocked(voiceStates);
	guildDataDirty.store(true);
}

void DiscordClient::applyGuildDetailsLocked(Guild &guild, Guild &details) {
	guild.name = details.name;
	guild.icon = details.icon;
	guild.ownerId = details.ownerId;
	guild.rules_channel_id = details.rules_channel_id;
	guild.description = details.description;
	guild.approximateMemberCount = details.approximateMemberCount;
	guild.approximatePresenceCount = details.approximatePresenceCount;
	if (!details.roles.empty()) {
		guild.roles = std::move(details.roles);
	}
}

struct DiscordClient::GuildLoad {
	std::string guildId;
	int pending = 3;
	bool failed = false;
	std::vector<std::string> myRoles;
	Guild details;
	std::vector<Channel> channels;
};

DiscordClient::GuildChannelsState DiscordClient::getGuildChannelsState(const std::string &guildId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	const Guild *guild = findGuildLocked(guildId);
	if (!guild || guild->channelsLoaded) {
		return GuildChannelsState::LOADED;
	}
	if (guildLoadInFlight == guildId) {
		return GuildChannelsState::LOADING;
	}
	if (guildLoadFailed == guildId) {
		return GuildChannelsState::FAILED;
	}
	return GuildChannelsState::UNLOADED;
}

bool DiscordClient::loadGuildChannels(const std::string &guildId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	Guild *target = findGuildLocked(guildId);
	if (!target || target->channelsLoaded || !guildLoadInFlight.empty() || token.empty()) {
		return false;
	}

	if (!hasRoomForGuildData()) {
		for (auto &guild : guilds) {
			if (guild.loadedOnDemand) {
				releaseGuildDetails(guild);
			}
		}
	}
	guildLoadInFlight = guildId;
	if (guildLoadFailed == guildId) {
		guildLoadFailed.clear();
	}
	Logger::log("[Guild] Loading channels for %s on demand", guildId.c_str());

	auto load = std::make_shared<GuildLoad>();
	load->guildId = guildId;
	const std::map<std::string, std::string> headers = {{"Authorization", token}};
	const std::string api = "https://discord.com/api/v10/";
	Network::NetworkManager &net = Network::NetworkManager::getInstance();

	auto finish = [this, load](bool ok) {
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		load->failed |= !ok;
		if (--load->pending == 0) {
			completeGuildLoadLocked(*load);
		}
	};

	net.enqueue(
	    api + "users/@me/guilds/" + guildId + "/member", "GET", "", Network::RequestPriority::INTERACTIVE,
	    [load, finish](Network::HttpResponse &resp) {
		    bool ok = resp.success && resp.statusCode == 200;
		    if (ok) {
			    rapidjson::Document doc;
			    doc.Parse(resp.body.c_str());
			    ok = !doc.HasParseError() && doc.IsObject();
			    if (ok) {
				    load->myRoles = parseMemberObject(doc, "").role_ids;
			    }
		    }
		    finish(ok);
	    },
	    headers);

	net.enqueue(
	    api + "guilds/" + guildId + "?with_counts=true", "GET", "", Network::RequestPriority::INTERACTIVE,
	    [this, load, finish](Network::HttpResponse &resp) { finish(parseGuildBody(resp, load->details)); }, headers);

	net.enqueue(
	    api + "guilds/" + guildId + "/channels", "GET", "", Network::RequestPriority::INTERACTIVE,
	    [this, load, finish](Network::HttpResponse &resp) {
		    bool fits = hasRoomForGuildData();
		    bool ok = resp.success && resp.statusCode == 200 && fits;
		    if (ok) {
			    Network::JsonInputStream stream(resp.body.data(), resp.body.size());
			    JsonPull p(stream);
			    Utils::Json::ScratchDocument scratch;
			    ok = parseChannelArray(p, scratch.get(), load->channels, fits) && fits;
			    load->channels.shrink_to_fit();
		    }
		    finish(ok);
	    },
	    headers);
	return true;
}

void DiscordClient::completeGuildLoadLocked(GuildLoad &load) {
	guildLoadInFlight.clear();
	Guild *guild = findGuildLocked(load.guildId);
	if (!guild || guild->channelsLoaded) {
		return;
	}
	if (load.failed) {
		guildLoadFailed = load.guildId;
		Logger::log("[Guild] Could not load channels for %s", load.guildId.c_str());
		guildDataDirty.store(true);
		return;
	}

	applyGuildDetailsLocked(*guild, load.details);
	guild->myRoles = std::move(load.myRoles);
	guild->channels = std::move(load.channels);
	guild->channelsLoaded = true;
	guild->loadedOnDemand = true;
	computeViewable(*guild, currentUser.id);
	Logger::log("[Guild] Loaded %zu channels for %s on demand", guild->channels.size(), load.guildId.c_str());
	guildDataDirty.store(true);
}

void DiscordClient::handleChannelCreateUpdate(const rapidjson::Value &d, const std::string &guildIdOverride) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);

	Channel channel;
	parseChannelObject(d, channel);

	if (channel.type == 1 || channel.type == 3) {
		bool found = false;
		for (auto &pc : privateChannels) {
			if (pc.id == channel.id) {
				pc = channel;
				found = true;
				break;
			}
		}
		if (!found) {
			privateChannels.insert(privateChannels.begin(), channel);
		}
		privateChannelsDirty.store(true);
		Logger::log("Updated DM channel %s (%s)", channel.name.c_str(), channel.id.c_str());
	} else if (d.HasMember("guild_id") || !guildIdOverride.empty()) {
		// Threads in a Thread List Sync carry the guild only on the envelope.
		std::string guildId = guildIdOverride.empty() ? Utils::Json::getString(d, "guild_id") : guildIdOverride;

		for (auto &guild : guilds) {
			if (guild.id == guildId) {
				// A lone channel would pass for the whole list.
				if (!guild.channelsLoaded) {
					break;
				}
				bool found = false;
				for (auto &c : guild.channels) {
					if (c.id == channel.id) {
						c = channel;
						found = true;
						break;
					}
				}
				if (!found) {
					guild.channels.push_back(channel);
				}

				uint64_t finalPerms = computeChannelPermissions(guild, channel, currentUser.id, guild.myRoles);
				for (auto &c : guild.channels) {
					if (c.id == channel.id) {
						c.viewable = (finalPerms & Permissions::VIEW_CHANNEL) != 0;
						break;
					}
				}

				Logger::log("Updated guild channel %s (%s) in guild %s", channel.name.c_str(), channel.id.c_str(),
				            guild.name.c_str());
				break;
			}
		}
	}
}

void DiscordClient::handleChannelDelete(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	std::string id = Utils::Json::getString(d, "id");

	for (auto it = privateChannels.begin(); it != privateChannels.end(); ++it) {
		if (it->id == id) {
			privateChannels.erase(it);
			Logger::log("Deleted DM channel %s", id.c_str());
			break;
		}
	}
}

void DiscordClient::handleTypingStart(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);

	std::string channelId = Utils::Json::getString(d, "channel_id");
	std::string userId = Utils::Json::getString(d, "user_id");

	Logger::log("TYPING_START: channel=%s user=%s (me=%s)", channelId.c_str(), userId.c_str(), currentUser.id.c_str());

	if (userId == currentUser.id) {
		return;
	}

	std::string displayName = userId;
	if (d.HasMember("member") && d["member"].IsObject()) {
		const rapidjson::Value &member = d["member"];
		std::string nick = Utils::Json::getString(member, "nick");
		if (!nick.empty()) {
			displayName = nick;
		} else if (member.HasMember("user") && member["user"].IsObject()) {
			const rapidjson::Value &user = member["user"];
			std::string globalName = Utils::Json::getString(user, "global_name");
			if (!globalName.empty()) {
				displayName = globalName;
			} else {
				displayName = Utils::Json::getString(user, "username");
			}
		}
	}

	TypingUser user;
	user.userId = userId;
	user.channelId = channelId;
	user.timestamp = time(NULL);
	user.displayName = displayName;

	auto &users = typingUsers[channelId];
	bool found = false;
	for (auto &u : users) {
		if (u.userId == userId) {
			u.timestamp = user.timestamp;
			found = true;
			Logger::log("Updated typing timestamp for user %s", userId.c_str());
			break;
		}
	}
	if (!found) {
		users.push_back(user);
		Logger::log("Added typing user %s to channel %s", userId.c_str(), channelId.c_str());
	}
}

void DiscordClient::handleMessageCreate(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	Message msg = parseSingleMessage(d);

	if (messageCallback) {
		messageCallback(msg);
	}

	if (typingUsers.find(msg.channelId) != typingUsers.end()) {
		auto &users = typingUsers[msg.channelId];
		for (auto it = users.begin(); it != users.end();) {
			if (it->userId == msg.author.id) {
				it = users.erase(it);
			} else {
				++it;
			}
		}
	}

	// Nobody else can see it, so the channel gains nothing to be unread about.
	if (msg.isEphemeral()) {
		auto &held = ephemeralMessages[msg.channelId];
		held.push_back(msg);
		if (held.size() > MAX_EPHEMERAL_PER_CHANNEL) {
			held.erase(held.begin());
		}
		return;
	}

	if (!msg.channelId.empty() && !msg.id.empty()) {
		std::string gId = getGuildIdFromChannel(msg.channelId);
		bool updatedChannel = false;
		if (!gId.empty() && gId != "DM") {
			for (auto &guild : guilds) {
				if (guild.id != gId) {
					continue;
				}
				for (auto &ch : guild.channels) {
					if (ch.id == msg.channelId) {
						ch.last_message_id = msg.id;
						updatedChannel = true;
						break;
					}
				}
				break;
			}
		}
		if (!updatedChannel) {
			for (auto it = privateChannels.begin(); it != privateChannels.end(); ++it) {
				if (it->id == msg.channelId) {
					it->last_message_id = msg.id;
					Channel ch = *it;
					privateChannels.erase(it);
					privateChannels.insert(privateChannels.begin(), ch);
					privateChannelsDirty.store(true);
					break;
				}
			}
		}
	}

	if (msg.channelId.empty() || msg.id.empty()) {
		return;
	}

	if (msg.author.id == currentUser.id) {
		auto &rs = readStates[msg.channelId];
		rs.channelId = msg.channelId;
		rs.lastReadMessageId = msg.id;
		rs.mentionCount = 0;
		readStateDirtyChannelId = msg.channelId;
		readStateDirty.store(true);
		return;
	}

	std::string guildId = getGuildIdFromChannel(msg.channelId);
	bool isPrivate = guildId.empty() || guildId == "DM";

	bool mentioned = isUserMentioned(msg);

	const GuildNotificationSettings *gs = nullptr;
	if (!isPrivate) {
		auto gsIt = notificationSettings.find(guildId);
		if (gsIt != notificationSettings.end()) {
			gs = &gsIt->second;
		}
	}

	bool playSound = false;
	if (isPrivate) {
		playSound = true;
	} else {
		bool isMuted = gs ? gs->muted : false;
		int notifyLevel = gs ? gs->messageNotifications : 0;

		if (gs) {
			auto chIt = gs->channelOverrides.find(msg.channelId);
			if (chIt != gs->channelOverrides.end()) {
				if (chIt->second.muted) {
					isMuted = true;
				}
				if (chIt->second.messageNotifications != 3) {
					notifyLevel = chIt->second.messageNotifications;
				}
			}
		}

		if (!isMuted) {
			if (notifyLevel == 0) {
				playSound = true;
			} else if (notifyLevel == 1 && mentioned) {
				playSound = true;
			}
		}
	}

	if (playSound && Config::getInstance().isNotificationSoundEnabled()) {
		Utils::SoundPlayer::getInstance().play(Utils::Sound::NOTIFICATION);
	}

	// Any message in an unmuted private channel counts towards the badge.
	if (mentioned || isPrivate) {
		auto &rs = readStates[msg.channelId];
		rs.channelId = msg.channelId;
		rs.mentionCount++;
	}
	readStateDirty.store(true);
}

void DiscordClient::handleMessageUpdate(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	Message msg = parseSingleMessage(d);

	auto held = ephemeralMessages.find(msg.channelId);
	if (held != ephemeralMessages.end()) {
		for (auto &m : held->second) {
			if (m.id == msg.id) {
				m = msg;
				break;
			}
		}
	}

	if (messageUpdateCallback) {
		messageUpdateCallback(msg);
	}
}

std::vector<Message> DiscordClient::getEphemeralMessages(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	auto held = ephemeralMessages.find(channelId);
	return held == ephemeralMessages.end() ? std::vector<Message>() : held->second;
}

void DiscordClient::forgetEphemeralMessage(const std::string &channelId, const std::string &messageId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	auto held = ephemeralMessages.find(channelId);
	if (held == ephemeralMessages.end()) {
		return;
	}
	auto &list = held->second;
	list.erase(std::remove_if(list.begin(), list.end(), [&](const Message &m) { return m.id == messageId; }),
	           list.end());
	if (list.empty()) {
		ephemeralMessages.erase(held);
	}
}

void DiscordClient::handleInteractionResult(const rapidjson::Value &d, InteractionResult result) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (interactionCallback) {
		interactionCallback(Utils::Json::getString(d, "nonce"), result);
	}
}

void DiscordClient::handleModalCreate(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	Modal modal;
	modal.id = Utils::Json::getString(d, "id");
	modal.channelId = Utils::Json::getString(d, "channel_id");
	modal.customId = Utils::Json::getString(d, "custom_id");
	modal.title = Utils::Json::getString(d, "title");
	modal.nonce = Utils::Json::getString(d, "nonce");
	modal.applicationId = Utils::Json::getString(d, "application_id");
	if (modal.applicationId.empty() && d.HasMember("application") && d["application"].IsObject()) {
		modal.applicationId = Utils::Json::getString(d["application"], "id");
	}

	if (d.HasMember("components") && d["components"].IsArray()) {
		for (const auto &comp : d["components"].GetArray()) {
			const int type = Utils::Json::getInt(comp, "type");
			if (type == ComponentType::TEXT_DISPLAY) {
				ModalField field;
				field.wrapperType = ComponentType::TEXT_DISPLAY;
				field.label = Utils::Json::getString(comp, "content");
				modal.fields.push_back(std::move(field));
			} else if (type == ComponentType::ACTION_ROW && comp.HasMember("components") &&
			           comp["components"].IsArray()) {
				for (const auto &inner : comp["components"].GetArray()) {
					ModalField field = parseModalInput(inner);
					field.wrapperType = ComponentType::ACTION_ROW;
					modal.fields.push_back(std::move(field));
				}
			} else if (type == ComponentType::LABEL && comp.HasMember("component") && comp["component"].IsObject()) {
				ModalField field = parseModalInput(comp["component"]);
				field.wrapperType = ComponentType::LABEL;
				field.label = Utils::Json::getString(comp, "label");
				field.description = Utils::Json::getString(comp, "description");
				modal.fields.push_back(std::move(field));
			}
		}
	}

	if (modalCallback) {
		modalCallback(modal);
	}
}

void DiscordClient::handleMessageDelete(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	std::string id = Utils::Json::getString(d, "id");

	forgetEphemeralMessage(Utils::Json::getString(d, "channel_id"), id);

	if (messageDeleteCallback) {
		messageDeleteCallback(id);
	}
}

void DiscordClient::handleReactionAdd(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	std::string channelId = Utils::Json::getString(d, "channel_id");
	std::string messageId = Utils::Json::getString(d, "message_id");
	std::string userId = Utils::Json::getString(d, "user_id");

	Emoji emoji;
	if (d.HasMember("emoji") && d["emoji"].IsObject()) {
		emoji = parseEmojiObject(d["emoji"]);
	}

	// Already shown when requested; a stale echo would undo a later toggle.
	if (userId == currentUser.id && hasQueuedReactionLocked(messageId, emoji)) {
		return;
	}
	if (messageReactionAddCallback) {
		messageReactionAddCallback(channelId, messageId, userId, emoji);
	}
}

void DiscordClient::handleReactionRemove(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	std::string channelId = Utils::Json::getString(d, "channel_id");
	std::string messageId = Utils::Json::getString(d, "message_id");
	std::string userId = Utils::Json::getString(d, "user_id");

	Emoji emoji;
	if (d.HasMember("emoji") && d["emoji"].IsObject()) {
		emoji = parseEmojiObject(d["emoji"]);
	}

	if (userId == currentUser.id && hasQueuedReactionLocked(messageId, emoji)) {
		return;
	}
	if (messageReactionRemoveCallback) {
		messageReactionRemoveCallback(channelId, messageId, userId, emoji);
	}
}

void DiscordClient::handlePollVote(const rapidjson::Value &d, bool added) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (!pollVoteCallback) {
		return;
	}
	pollVoteCallback(Utils::Json::getString(d, "channel_id"), Utils::Json::getString(d, "message_id"),
	                 Utils::Json::getString(d, "user_id"), Utils::Json::getInt(d, "answer_id"), added);
}

void DiscordClient::handlePresenceUpdate(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (!d.HasMember("user") || !d["user"].IsObject()) {
		return;
	}
	std::string userId = Utils::Json::getString(d["user"], "id");

	if (userId == currentUser.id) {
		std::string statusStr = Utils::Json::getString(d, "status");
		currentUser.status = stringToStatus(statusStr);
		Logger::log("[Gateway] Own presence updated via PRESENCE_UPDATE to %s", statusStr.c_str());
	}
}

static std::string parseMuteEndTime(const rapidjson::Value &obj) {
	if (!obj.HasMember("mute_config") || !obj["mute_config"].IsObject()) {
		return "";
	}
	const rapidjson::Value &mc = obj["mute_config"];
	if (!mc.HasMember("end_time") || !mc["end_time"].IsString()) {
		return "";
	}
	return mc["end_time"].GetString();
}

static void parseChannelOverride(const rapidjson::Value &ov, ChannelNotificationOverride &co) {
	co.channelId = Utils::Json::getString(ov, "channel_id");
	co.muted = Utils::Json::getBool(ov, "muted");
	co.muteEndTime = parseMuteEndTime(ov);
	co.messageNotifications = Utils::Json::getInt(ov, "message_notifications", 3);
	co.flags = Utils::Json::getInt(ov, "flags", 0);
}

static void parseChannelOverrides(const rapidjson::Value &obj,
                                  std::map<std::string, ChannelNotificationOverride> &out) {
	if (!obj.HasMember("channel_overrides") || !obj["channel_overrides"].IsArray()) {
		return;
	}
	const rapidjson::Value &overrides = obj["channel_overrides"];
	for (rapidjson::SizeType j = 0; j < overrides.Size(); j++) {
		ChannelNotificationOverride co;
		parseChannelOverride(overrides[j], co);
		if (!co.channelId.empty()) {
			out[co.channelId] = co;
		}
	}
}

void DiscordClient::parseUserGuildSettings(const rapidjson::Value &entry,
                                           std::map<std::string, GuildNotificationSettings> &out) {
	GuildNotificationSettings gs;
	gs.guildId = Utils::Json::getString(entry, "guild_id");
	gs.muted = Utils::Json::getBool(entry, "muted");
	gs.muteEndTime = parseMuteEndTime(entry);
	gs.messageNotifications = Utils::Json::getInt(entry, "message_notifications", 3);
	gs.flags = Utils::Json::getInt(entry, "flags", 0);
	gs.suppressEveryone = Utils::Json::getBool(entry, "suppress_everyone");
	gs.suppressRoles = Utils::Json::getBool(entry, "suppress_roles");

	parseChannelOverrides(entry, gs.channelOverrides);
	if (!gs.guildId.empty()) {
		out[gs.guildId] = std::move(gs);
	}
}

void DiscordClient::handleUserGuildSettingsUpdate(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	GuildNotificationSettings gs;
	gs.guildId = Utils::Json::getString(d, "guild_id");
	gs.muted = Utils::Json::getBool(d, "muted");
	gs.muteEndTime = parseMuteEndTime(d);
	gs.messageNotifications = Utils::Json::getInt(d, "message_notifications", 3);
	gs.flags = Utils::Json::getInt(d, "flags", 0);
	gs.suppressEveryone = Utils::Json::getBool(d, "suppress_everyone");
	gs.suppressRoles = Utils::Json::getBool(d, "suppress_roles");

	parseChannelOverrides(d, gs.channelOverrides);
	if (!gs.guildId.empty()) {
		auto it = notificationSettings.find(gs.guildId);
		if (gs.muted && gs.muteEndTime.empty() && it != notificationSettings.end() && it->second.muted &&
		    !it->second.muteEndTime.empty()) {
			gs.muteEndTime = it->second.muteEndTime;
		}
		if (it != notificationSettings.end()) {
			for (auto &[chId, co] : gs.channelOverrides) {
				if (co.muted && co.muteEndTime.empty()) {
					auto coIt = it->second.channelOverrides.find(chId);
					if (coIt != it->second.channelOverrides.end() && coIt->second.muted &&
					    !coIt->second.muteEndTime.empty()) {
						co.muteEndTime = coIt->second.muteEndTime;
					}
				}
			}
		}
		Logger::log("[Gateway] Updated notification settings for guild %s", gs.guildId.c_str());
		notificationSettings[gs.guildId] = std::move(gs);
	}
}

void DiscordClient::handleUserSettingsUpdate(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (d.HasMember("status") && d["status"].IsString()) {
		std::string statusStr = d["status"].GetString();
		currentUser.status = stringToStatus(statusStr);
		Logger::log("[Gateway] Own status updated via USER_SETTINGS_UPDATE to %s", statusStr.c_str());
	}
}

void DiscordClient::handleSessionsReplace(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (d.IsArray()) {
		for (rapidjson::SizeType i = 0; i < d.Size(); i++) {
			const rapidjson::Value &s = d[i];
			if (Utils::Json::getString(s, "session_id") == sessionId) {
				std::string statusStr = Utils::Json::getString(s, "status");
				currentUser.status = stringToStatus(statusStr);
				Logger::log("[Gateway] Own status updated via SESSIONS_REPLACE to %s", statusStr.c_str());
				break;
			}
		}
	}
}

void DiscordClient::handleResumed() {
	Logger::log("[Gateway] Session Resumed");
	if (connectionCallback) {
		connectionCallback();
	}
}

void DiscordClient::handleMessageAck(const rapidjson::Value &d) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	std::string channelId = Utils::Json::getString(d, "channel_id");
	std::string messageId = Utils::Json::getString(d, "message_id");
	if (channelId.empty()) {
		return;
	}
	auto &rs = readStates[channelId];
	rs.channelId = channelId;
	if (!messageId.empty()) {
		// A manual ack may move the cursor backwards ("mark as unread").
		bool manual = Utils::Json::getBool(d, "manual");
		if (manual || UI::MessageUtils::isNewerSnowflake(messageId, rs.lastReadMessageId)) {
			rs.lastReadMessageId = messageId;
		}
	}
	int mentionCount = Utils::Json::getInt(d, "mention_count", -1);
	if (mentionCount >= 0) {
		rs.mentionCount = mentionCount;
	} else {
		rs.mentionCount = 0;
	}
	readStateDirtyChannelId = channelId;
	readStateDirty.store(true);
}

void DiscordClient::markChannelRead(const std::string &channelId, const std::string &messageId) {
	if (channelId.empty() || messageId.empty() || token.empty()) {
		return;
	}
	std::string storedToken;
	int lastViewed = 0;
	int flags = 0;
	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		auto &rs = readStates[channelId];
		rs.channelId = channelId;

		if (!UI::MessageUtils::isNewerSnowflake(messageId, rs.lastReadMessageId)) {
			return;
		}
		rs.lastReadMessageId = messageId;
		rs.mentionCount = 0;
		storedToken = rs.ackToken;
		readStateDirtyChannelId = channelId;
		readStateDirty.store(true);

		// last_viewed: days since 2015-01-01 (Discord epoch)
		lastViewed = static_cast<int>((time(nullptr) - 1420070400LL) / 86400);

		// Read state flags: 1=GUILD_CHANNEL, 2=THREAD (types 10/11/12)
		auto gIt = channelToGuildCache.find(channelId);
		if (gIt != channelToGuildCache.end() && !gIt->second.empty()) {
			flags |= 1;
			for (const auto &guild : guilds) {
				if (guild.id != gIt->second) {
					continue;
				}
				for (const auto &ch : guild.channels) {
					if (ch.id != channelId) {
						continue;
					}
					if (ch.type == 10 || ch.type == 11 || ch.type == 12) {
						flags |= 2;
					}
					break;
				}
				break;
			}
		}
	}

	std::string tokenJson = storedToken.empty() ? "null" : "\"" + storedToken + "\"";
	std::string body = "{\"token\":" + tokenJson + ",\"last_viewed\":" + std::to_string(lastViewed) +
	                   ",\"flags\":" + std::to_string(flags) + "}";
	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages/" + messageId + "/ack";
	Network::NetworkManager::getInstance().enqueue(
	    url, "POST", body, Network::RequestPriority::BACKGROUND,
	    [this, channelId](const Network::HttpResponse &resp) {
		    if (resp.success && resp.statusCode == 200 && !resp.body.empty()) {
			    rapidjson::Document doc;
			    doc.Parse(resp.body.c_str());
			    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("token") && doc["token"].IsString()) {
				    std::string newToken = doc["token"].GetString();
				    if (!newToken.empty()) {
					    std::lock_guard<std::recursive_mutex> lock(clientMutex);
					    readStates[channelId].ackToken = newToken;
				    }
			    }
		    } else if (!resp.success || resp.statusCode != 200) {
			    Logger::log("[ACK] Channel %s mark failed: %d %s", channelId.c_str(), resp.statusCode,
			                resp.error.c_str());
		    }
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

void DiscordClient::markChannelReadLatest(const std::string &channelId, const std::string &seenMessageId) {
	std::string lastMsgId = seenMessageId;
	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		const Channel *channel = getChannelPtr(channelId);
		if (channel && UI::MessageUtils::isNewerSnowflake(channel->last_message_id, lastMsgId)) {
			lastMsgId = channel->last_message_id;
		}
	}
	if (!lastMsgId.empty()) {
		markChannelRead(channelId, lastMsgId);
	}
}

void DiscordClient::markGuildRead(const std::string &guildId) {
	if (token.empty() || guildId.empty()) {
		return;
	}

	rapidjson::Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();
	rapidjson::Value rsArray(rapidjson::kArrayType);

	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		for (const auto &guild : guilds) {
			if (guild.id != guildId) {
				continue;
			}
			for (const auto &ch : guild.channels) {
				if (ch.type == 2 || ch.type == 4 || ch.type == 13) {
					continue;
				}
				if (ch.last_message_id.empty()) {
					continue;
				}

				auto rsIt = readStates.find(ch.id);
				bool isUnread = (rsIt == readStates.end()) ||
				                UI::MessageUtils::isNewerSnowflake(ch.last_message_id, rsIt->second.lastReadMessageId);
				if (!isUnread) {
					continue;
				}

				rapidjson::Value item(rapidjson::kObjectType);
				item.AddMember("channel_id", rapidjson::Value(ch.id.c_str(), alloc), alloc);
				item.AddMember("message_id", rapidjson::Value(ch.last_message_id.c_str(), alloc), alloc);
				item.AddMember("read_state_type", 0, alloc);
				rsArray.PushBack(item, alloc);

				auto &rs = readStates[ch.id];
				rs.channelId = ch.id;
				rs.lastReadMessageId = ch.last_message_id;
				rs.mentionCount = 0;
			}
			break;
		}
		readStateDirty.store(true);
	}

	if (rsArray.Empty()) {
		return;
	}

	doc.AddMember("read_states", rsArray, alloc);
	rapidjson::StringBuffer buf;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
	doc.Accept(writer);

	Network::NetworkManager::getInstance().enqueue(
	    "https://discord.com/api/v10/read-states/ack-bulk", "POST", buf.GetString(),
	    Network::RequestPriority::INTERACTIVE,
	    [guildId](const Network::HttpResponse &resp) {
		    if (resp.success) {
			    Logger::log("[API] Guild %s marked as read", guildId.c_str());
		    } else {
			    Logger::log("[API] Failed to mark guild as read: %d %s", resp.statusCode, resp.error.c_str());
		    }
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

User DiscordClient::parseUserObject(const rapidjson::Value &uObj) {
	User u;
	u.id = Utils::Json::getString(uObj, "id");
	u.username = Utils::Json::getString(uObj, "username");
	u.global_name = Utils::Json::getString(uObj, "global_name");
	u.avatar = Utils::Json::getString(uObj, "avatar");
	u.discriminator = Utils::Json::getString(uObj, "discriminator");
	u.bot = Utils::Json::getBool(uObj, "bot");
	return u;
}

Member DiscordClient::parseMemberObject(const rapidjson::Value &mObj, const std::string &userId) {
	Member member;
	member.user_id = userId;
	member.nickname = Utils::Json::getString(mObj, "nick");
	if (mObj.HasMember("user") && mObj["user"].IsObject()) {
		const rapidjson::Value &user = mObj["user"];
		member.username = Utils::Json::getString(user, "username");
		member.globalName = Utils::Json::getString(user, "global_name");
		member.avatar = Utils::Json::getString(user, "avatar");
	}
	if (mObj.HasMember("roles") && mObj["roles"].IsArray()) {
		const rapidjson::Value &roles = mObj["roles"];
		for (rapidjson::SizeType i = 0; i < roles.Size(); i++) {
			if (roles[i].IsString()) {
				member.role_ids.push_back(roles[i].GetString());
			}
		}
	}
	return member;
}

namespace {

constexpr int ATTACHMENT_FLAG_SPOILER = 1 << 3;
constexpr int SEPARATOR_SPACING_LARGE = 2;
constexpr int CHAT_INPUT_COMMAND_MESSAGE = 20;

bool hasArray(const rapidjson::Value &value, const char *key) { return value.HasMember(key) && value[key].IsArray(); }

bool hasObject(const rapidjson::Value &value, const char *key) { return value.HasMember(key) && value[key].IsObject(); }

bool isSpoiler(const rapidjson::Value &value) {
	return value.HasMember("spoiler") && value["spoiler"].IsBool() && value["spoiler"].GetBool();
}

std::string withQuery(const std::string &url, const char *query) {
	return url + (url.find('?') == std::string::npos ? "?" : "&") + query;
}

// Tenor keeps every rendition of a GIF under one id, and the last five
// characters of the id select the rendition. AAAAM is the 220px GIF.
std::string tenorGifUrl(const std::string &url) {
	constexpr size_t CODE_LENGTH = 5;
	const size_t host = url.find(".tenor.com/");
	if (host == std::string::npos) {
		return "";
	}
	const size_t idStart = host + strlen(".tenor.com/");
	const size_t idEnd = url.find('/', idStart);
	const size_t extension = url.rfind('.');
	if (idEnd == std::string::npos || idEnd - idStart <= CODE_LENGTH || extension < idEnd) {
		return "";
	}
	return url.substr(0, idEnd - CODE_LENGTH) + "AAAAM" + url.substr(idEnd, extension - idEnd) + ".gif";
}

// A gifv embed only points at video. The proxy transcodes on request, but its
// URLs are signed, so Tenor's own GIF rendition cannot go through it.
std::string gifvUrl(const Embed &embed, const rapidjson::Value &video) {
	std::string tenorUrl = tenorGifUrl(embed.thumbnail_url);
	if (tenorUrl.empty()) {
		tenorUrl = tenorGifUrl(Utils::Json::getString(video, "url"));
	}
	if (!tenorUrl.empty()) {
		return tenorUrl;
	}
	const std::string proxyUrl = Utils::Json::getString(video, "proxy_url");
	if (proxyUrl.empty() || Utils::Gif::hasGifPath(embed.thumbnail_url)) {
		return "";
	}
	return withQuery(proxyUrl, "format=gif");
}

} // namespace

Embed DiscordClient::parseEmbedObject(const rapidjson::Value &eObj) {
	Embed embed;
	embed.title = Utils::Json::getString(eObj, "title");
	embed.description = Utils::Json::getString(eObj, "description");
	embed.url = Utils::Json::getString(eObj, "url");
	embed.type = Utils::Json::getString(eObj, "type");
	embed.color = Utils::Json::getInt(eObj, "color");
	embed.timestamp = Utils::Json::getString(eObj, "timestamp");
	if (eObj.HasMember("author") && eObj["author"].IsObject()) {
		embed.author_name = Utils::Json::getString(eObj["author"], "name");
		embed.author_icon_url = Utils::Json::getString(eObj["author"], "icon_url");
	}
	if (eObj.HasMember("footer") && eObj["footer"].IsObject()) {
		embed.footer_text = Utils::Json::getString(eObj["footer"], "text");
		embed.footer_icon_url = Utils::Json::getString(eObj["footer"], "icon_url");
	}
	if (eObj.HasMember("provider") && eObj["provider"].IsObject()) {
		embed.provider_name = Utils::Json::getString(eObj["provider"], "name");
	}
	if (eObj.HasMember("image") && eObj["image"].IsObject()) {
		const rapidjson::Value &img = eObj["image"];
		embed.image_url = Utils::Json::getString(img, "url");
		embed.image_proxy_url = Utils::Json::getString(img, "proxy_url");
		embed.image_width = Utils::Json::getInt(img, "width");
		embed.image_height = Utils::Json::getInt(img, "height");
	}
	if (eObj.HasMember("thumbnail") && eObj["thumbnail"].IsObject()) {
		const rapidjson::Value &thumb = eObj["thumbnail"];
		embed.thumbnail_url = Utils::Json::getString(thumb, "url");
		embed.thumbnail_proxy_url = Utils::Json::getString(thumb, "proxy_url");
		embed.thumbnail_width = Utils::Json::getInt(thumb, "width");
		embed.thumbnail_height = Utils::Json::getInt(thumb, "height");
	}
	if (embed.type == "gifv" && eObj.HasMember("video") && eObj["video"].IsObject()) {
		embed.gif_url = gifvUrl(embed, eObj["video"]);
	}
	if (eObj.HasMember("fields") && eObj["fields"].IsArray()) {
		const rapidjson::Value &fields = eObj["fields"];
		for (rapidjson::SizeType f = 0; f < fields.Size() && f < 10; f++) {
			const rapidjson::Value &fObj = fields[f];
			EmbedField field;
			field.name = Utils::Json::getString(fObj, "name");
			field.value = Utils::Json::getString(fObj, "value");
			field.isInline = fObj.HasMember("inline") && fObj["inline"].IsBool() ? fObj["inline"].GetBool() : false;
			embed.fields.push_back(field);
		}
	}
	// A gifv shows as the GIF alone, so any text it carries would only bring the frame back.
	if (embed.isBareGif()) {
		embed.title.clear();
		embed.description.clear();
		embed.author_name.clear();
		embed.footer_text.clear();
		embed.provider_name.clear();
		embed.fields.clear();
	}
	return embed;
}

Attachment DiscordClient::parseAttachmentObject(const rapidjson::Value &aObj) {
	Attachment attachment;
	attachment.id = Utils::Json::getString(aObj, "id");
	attachment.filename = Utils::Json::getString(aObj, "filename");
	attachment.url = Utils::Json::getString(aObj, "url");
	attachment.proxy_url = Utils::Json::getString(aObj, "proxy_url");
	attachment.size = Utils::Json::getInt(aObj, "size");
	attachment.width = Utils::Json::getInt(aObj, "width");
	attachment.height = Utils::Json::getInt(aObj, "height");
	attachment.content_type = Utils::Json::getString(aObj, "content_type");
	// The prefix is how clients mark a spoiler on upload; the docs only describe the flag.
	attachment.spoiler =
	    (Utils::Json::getInt(aObj, "flags") & ATTACHMENT_FLAG_SPOILER) || attachment.filename.rfind("SPOILER_", 0) == 0;
	if ((size_t)attachment.size > Utils::Image::maxAnimatedBytes() && Utils::Gif::hasGifPath(attachment.proxy_url)) {
		attachment.proxy_url = withQuery(attachment.proxy_url, "format=jpeg");
	}
	return attachment;
}

Emoji DiscordClient::parseEmojiObject(const rapidjson::Value &eObj) {
	Emoji emoji;
	emoji.id = Utils::Json::getString(eObj, "id");
	emoji.name = Utils::Json::getString(eObj, "name");
	emoji.animated = eObj.HasMember("animated") && eObj["animated"].IsBool() && eObj["animated"].GetBool();
	return emoji;
}

MessageControl DiscordClient::parseControl(const rapidjson::Value &component, bool hidden) {
	MessageControl control;
	control.type = Utils::Json::getInt(component, "type");
	control.customId = Utils::Json::getString(component, "custom_id");
	control.disabled = Utils::Json::getBool(component, "disabled");
	control.hidden = hidden;

	if (control.isButton()) {
		control.style = Utils::Json::getInt(component, "style");
		control.label = Utils::Json::getString(component, "label");
		control.url = Utils::Json::getString(component, "url");
		if (hasObject(component, "emoji")) {
			control.emoji = parseEmojiObject(component["emoji"]);
		}
		return control;
	}

	control.label = Utils::Json::getString(component, "placeholder");
	control.minValues = Utils::Json::getInt(component, "min_values", 1);
	control.maxValues = Utils::Json::getInt(component, "max_values", 1);

	if (hasArray(component, "options")) {
		for (const auto &item : component["options"].GetArray()) {
			SelectOption option;
			option.label = Utils::Json::getString(item, "label");
			option.value = Utils::Json::getString(item, "value");
			option.description = Utils::Json::getString(item, "description");
			if (hasObject(item, "emoji")) {
				option.emoji = parseEmojiObject(item["emoji"]);
			}
			if (Utils::Json::getBool(item, "default")) {
				control.chosen.push_back(option.value);
				control.chosenLabel += (control.chosenLabel.empty() ? "" : ", ") + option.label;
			}
			control.options.push_back(option);
		}
	}
	if (hasArray(component, "default_values")) {
		for (const auto &item : component["default_values"].GetArray()) {
			control.chosen.push_back(Utils::Json::getString(item, "id"));
		}
	}
	if (hasArray(component, "channel_types")) {
		for (const auto &item : component["channel_types"].GetArray()) {
			if (item.IsInt()) {
				control.channelTypes.push_back(item.GetInt());
			}
		}
	}
	return control;
}

ModalField DiscordClient::parseModalInput(const rapidjson::Value &component) {
	ModalField field;
	const int type = Utils::Json::getInt(component, "type");
	field.required = Utils::Json::getBool(component, "required", true);

	if (type == ComponentType::TEXT_INPUT) {
		field.control.type = ComponentType::TEXT_INPUT;
		field.control.style = Utils::Json::getInt(component, "style", 1);
		field.control.customId = Utils::Json::getString(component, "custom_id");
		field.label = Utils::Json::getString(component, "label");
		field.placeholder = Utils::Json::getString(component, "placeholder");
		field.value = Utils::Json::getString(component, "value");
		field.minLength = Utils::Json::getInt(component, "min_length", 0);
		field.maxLength = Utils::Json::getInt(component, "max_length", 4000);
		return field;
	}

	if (type == ComponentType::CHECKBOX) {
		field.control.type = ComponentType::CHECKBOX;
		field.control.customId = Utils::Json::getString(component, "custom_id");
		field.checked = Utils::Json::getBool(component, "default", false);
		field.required = false;
		return field;
	}

	field.control = parseControl(component, false);
	field.placeholder = field.control.label;
	if (type == ComponentType::RADIO_GROUP) {
		field.control.minValues = field.required ? 1 : 0;
		field.control.maxValues = 1;
	} else if (type == ComponentType::CHECKBOX_GROUP) {
		const int defaultMax = field.control.options.empty() ? 1 : (int)field.control.options.size();
		field.control.minValues = Utils::Json::getInt(component, "min_values", field.required ? 1 : 0);
		if (!field.required && !component.HasMember("min_values")) {
			field.control.minValues = 0;
		}
		field.control.maxValues = Utils::Json::getInt(component, "max_values", defaultMax);
	} else {
		const int minVals = Utils::Json::getInt(component, "min_values", field.required ? 1 : 0);
		field.control.minValues = field.required ? std::max(1, minVals) : 0;
	}
	return field;
}

void DiscordClient::parseComponents(const rapidjson::Value &components, Message &msg, std::vector<MessageBlock> &blocks,
                                    bool spoiler) {
	const auto addMedia = [&msg](const rapidjson::Value &media, bool hidden, bool image) {
		Attachment attachment = parseAttachmentObject(media);
		const std::string path = attachment.url.substr(0, attachment.url.find('?'));
		attachment.filename = path.substr(path.rfind('/') + 1);
		attachment.spoiler = attachment.spoiler || hidden;
		if (image && attachment.content_type.empty()) {
			attachment.content_type = "image/";
		}
		msg.attachments.push_back(attachment);
		return (int)msg.attachments.size() - 1;
	};
	const auto addText = [&msg](MessageBlock &block, const rapidjson::Value &display, bool hidden) {
		const std::string raw = Utils::Json::getString(display, "content");
		const std::string text = hidden ? "||" + raw + "||" : raw;
		block.text += (block.text.empty() ? "" : "\n") + UI::MessageUtils::formatMentions(text, msg);
		msg.content += (msg.content.empty() ? "" : "\n") + text;
	};

	for (rapidjson::SizeType c = 0; c < components.Size(); c++) {
		const rapidjson::Value &component = components[c];
		const int type = Utils::Json::getInt(component, "type");
		const bool hidden = spoiler || isSpoiler(component);

		MessageBlock block;
		block.spoiler = hidden;

		if (type == ComponentType::TEXT_DISPLAY) {
			addText(block, component, hidden);
		} else if (type == ComponentType::SECTION && hasArray(component, "components")) {
			for (const auto &display : component["components"].GetArray()) {
				addText(block, display, hidden);
			}
			if (hasObject(component, "accessory")) {
				const rapidjson::Value &accessory = component["accessory"];
				const int accessoryType = Utils::Json::getInt(accessory, "type");
				if (accessoryType == ComponentType::THUMBNAIL && hasObject(accessory, "media")) {
					block.thumbnail = addMedia(accessory["media"], hidden || isSpoiler(accessory), true);
				} else if (accessoryType == ComponentType::BUTTON) {
					block.controls.push_back(parseControl(accessory, hidden));
				}
			}
		} else if (type == ComponentType::ACTION_ROW && hasArray(component, "components")) {
			block.kind = MessageBlock::Kind::CONTROLS;
			for (const auto &control : component["components"].GetArray()) {
				block.controls.push_back(parseControl(control, hidden));
			}
		} else if (type == ComponentType::MEDIA_GALLERY && hasArray(component, "items")) {
			block.kind = MessageBlock::Kind::MEDIA;
			for (const auto &item : component["items"].GetArray()) {
				if (hasObject(item, "media")) {
					block.attachments.push_back(addMedia(item["media"], hidden || isSpoiler(item), true));
				}
			}
		} else if (type == ComponentType::FILE_DISPLAY && hasObject(component, "file")) {
			block.kind = MessageBlock::Kind::MEDIA;
			block.attachments.push_back(addMedia(component["file"], hidden, false));
			msg.attachments.back().filename = Utils::Json::getString(component, "name");
		} else if (type == ComponentType::SEPARATOR) {
			block.kind = MessageBlock::Kind::SEPARATOR;
			block.divider = Utils::Json::getBool(component, "divider", true);
			block.largeSpacing = Utils::Json::getInt(component, "spacing") == SEPARATOR_SPACING_LARGE;
		} else if (type == ComponentType::CONTAINER && hasArray(component, "components")) {
			block.kind = MessageBlock::Kind::CONTAINER;
			if (component.HasMember("accent_color") && component["accent_color"].IsInt()) {
				block.accentColor = component["accent_color"].GetInt();
			}
			parseComponents(component["components"], msg, block.children, hidden);
		} else {
			continue;
		}
		blocks.push_back(std::move(block));
	}
}

Message DiscordClient::parseSingleMessage(const rapidjson::Value &d) {
	Message msg;
	msg.id = Utils::Json::getString(d, "id");
	msg.content = Utils::Json::getString(d, "content");
	msg.timestamp = Utils::Json::getString(d, "timestamp");
	msg.edited_timestamp = Utils::Json::getString(d, "edited_timestamp");
	msg.channelId = Utils::Json::getString(d, "channel_id");
	msg.nonce = Utils::Json::getString(d, "nonce");

	if (d.HasMember("author") && d["author"].IsObject()) {
		msg.author = parseUserObject(d["author"]);
	}

	if (d.HasMember("member") && d["member"].IsObject()) {
		msg.member = parseMemberObject(d["member"], msg.author.id);
	}

	if (d.HasMember("embeds") && d["embeds"].IsArray()) {
		const rapidjson::Value &embeds = d["embeds"];
		for (rapidjson::SizeType e = 0; e < embeds.Size(); e++) {
			msg.embeds.push_back(parseEmbedObject(embeds[e]));
		}
	}

	for (const auto &embed : msg.embeds) {
		if (embed.type != "poll_result") {
			continue;
		}
		msg.hasPollResult = true;
		for (const auto &field : embed.fields) {
			if (field.name == "poll_question_text") {
				msg.pollResult.question = field.value;
			} else if (field.name == "total_votes") {
				msg.pollResult.totalVotes = atoi(field.value.c_str());
			} else if (field.name == "victor_answer_text") {
				msg.pollResult.winnerText = field.value;
				msg.pollResult.hasWinner = true;
			} else if (field.name == "victor_answer_votes") {
				msg.pollResult.winnerVotes = atoi(field.value.c_str());
			} else if (field.name == "victor_answer_emoji_id") {
				msg.pollResult.winnerEmoji.id = field.value;
			} else if (field.name == "victor_answer_emoji_name") {
				msg.pollResult.winnerEmoji.name = field.value;
			}
		}
		msg.embeds.clear();
		break;
	}

	if (d.HasMember("attachments") && d["attachments"].IsArray()) {
		const rapidjson::Value &attachments = d["attachments"];
		for (rapidjson::SizeType a = 0; a < attachments.Size(); a++) {
			msg.attachments.push_back(parseAttachmentObject(attachments[a]));
		}
	}

	if (d.HasMember("sticker_items") && d["sticker_items"].IsArray()) {
		const rapidjson::Value &stickers = d["sticker_items"];
		for (rapidjson::SizeType s = 0; s < stickers.Size(); s++) {
			const rapidjson::Value &sObj = stickers[s];
			Sticker sticker;
			sticker.id = Utils::Json::getString(sObj, "id");
			sticker.name = Utils::Json::getString(sObj, "name");
			sticker.format_type = Utils::Json::getInt(sObj, "format_type", 1);
			msg.stickers.push_back(sticker);
		}
	} else if (d.HasMember("stickers") && d["stickers"].IsArray()) {
		const rapidjson::Value &stickers = d["stickers"];
		for (rapidjson::SizeType s = 0; s < stickers.Size(); s++) {
			const rapidjson::Value &sObj = stickers[s];
			Sticker sticker;
			sticker.id = Utils::Json::getString(sObj, "id");
			sticker.name = Utils::Json::getString(sObj, "name");
			sticker.format_type = Utils::Json::getInt(sObj, "format_type", 1);
			msg.stickers.push_back(sticker);
		}
	}

	if (d.HasMember("reactions") && d["reactions"].IsArray()) {
		const rapidjson::Value &reactions = d["reactions"];
		for (rapidjson::SizeType r = 0; r < reactions.Size(); r++) {
			const rapidjson::Value &rObj = reactions[r];
			Reaction reaction;
			reaction.count = Utils::Json::getInt(rObj, "count");
			reaction.me = rObj.HasMember("me") && rObj["me"].IsBool() ? rObj["me"].GetBool() : false;

			if (rObj.HasMember("emoji") && rObj["emoji"].IsObject()) {
				reaction.emoji = parseEmojiObject(rObj["emoji"]);
			}
			msg.reactions.push_back(reaction);
		}
	}

	if (d.HasMember("poll") && d["poll"].IsObject()) {
		const rapidjson::Value &pObj = d["poll"];
		msg.hasPoll = true;
		msg.poll.expiry = Utils::Json::getString(pObj, "expiry");
		msg.poll.allowMultiselect = Utils::Json::getBool(pObj, "allow_multiselect");
		if (pObj.HasMember("question") && pObj["question"].IsObject()) {
			msg.poll.question = Utils::Json::getString(pObj["question"], "text");
		}

		if (pObj.HasMember("answers") && pObj["answers"].IsArray()) {
			const rapidjson::Value &answers = pObj["answers"];
			for (rapidjson::SizeType i = 0; i < answers.Size(); i++) {
				const rapidjson::Value &aObj = answers[i];
				PollAnswer answer;
				answer.id = Utils::Json::getInt(aObj, "answer_id", (int)i + 1);
				if (aObj.HasMember("poll_media") && aObj["poll_media"].IsObject()) {
					const rapidjson::Value &mObj = aObj["poll_media"];
					answer.text = Utils::Json::getString(mObj, "text");
					if (mObj.HasMember("emoji") && mObj["emoji"].IsObject()) {
						answer.emoji = parseEmojiObject(mObj["emoji"]);
					}
				}
				msg.poll.answers.push_back(answer);
			}
		}

		if (pObj.HasMember("results") && pObj["results"].IsObject()) {
			const rapidjson::Value &rObj = pObj["results"];
			msg.poll.finalized = Utils::Json::getBool(rObj, "is_finalized");
			if (rObj.HasMember("answer_counts") && rObj["answer_counts"].IsArray()) {
				const rapidjson::Value &counts = rObj["answer_counts"];
				for (rapidjson::SizeType i = 0; i < counts.Size(); i++) {
					int id = Utils::Json::getInt(counts[i], "id");
					for (auto &answer : msg.poll.answers) {
						if (answer.id != id) {
							continue;
						}
						answer.count = Utils::Json::getInt(counts[i], "count");
						answer.meVoted = Utils::Json::getBool(counts[i], "me_voted");
						break;
					}
				}
			}
		}
	}

	if (d.HasMember("mentions") && d["mentions"].IsArray()) {
		const rapidjson::Value &mentions = d["mentions"];
		for (rapidjson::SizeType i = 0; i < mentions.Size(); i++) {
			msg.mentions.push_back(parseUserObject(mentions[i]));
		}
	}

	if (d.HasMember("mention_everyone") && d["mention_everyone"].IsBool()) {
		msg.mentionEveryone = d["mention_everyone"].GetBool();
	}

	if (d.HasMember("mention_roles") && d["mention_roles"].IsArray()) {
		const rapidjson::Value &roles = d["mention_roles"];
		for (rapidjson::SizeType i = 0; i < roles.Size(); i++) {
			if (roles[i].IsString()) {
				msg.mentionRoles.push_back(roles[i].GetString());
			}
		}
	}

	msg.type = Utils::Json::getInt(d, "type");

	msg.flags = Utils::Json::getInt(d, "flags");
	msg.applicationId = Utils::Json::getString(d, "application_id");

	if (hasArray(d, "components")) {
		if (msg.isComponentsV2()) {
			msg.attachments.clear();
		}
		parseComponents(d["components"], msg, msg.blocks, false);
	}

	const char *interactionKey = hasObject(d, "interaction_metadata") ? "interaction_metadata" : "interaction";
	if (hasObject(d, interactionKey)) {
		const rapidjson::Value &interaction = d[interactionKey];
		const std::string name = Utils::Json::getString(interaction, "name");
		if (!name.empty()) {
			msg.interactionName = msg.type == CHAT_INPUT_COMMAND_MESSAGE ? "/" + name : name;
		}
		if (hasObject(interaction, "user")) {
			const User user = parseUserObject(interaction["user"]);
			msg.interactionUser = user.global_name.empty() ? user.username : user.global_name;
		}
	}

	if (d.HasMember("message_snapshots") && d["message_snapshots"].IsArray()) {
		const rapidjson::Value &snapshots = d["message_snapshots"];
		if (snapshots.Size() > 0 && snapshots[0].HasMember("message") && snapshots[0]["message"].IsObject()) {
			msg.isForwarded = true;
			const rapidjson::Value &innerMsg = snapshots[0]["message"];
			if (msg.content.empty()) {
				msg.content = Utils::Json::getString(innerMsg, "content");
			}
			if (innerMsg.HasMember("author") && innerMsg["author"].IsObject()) {
				const rapidjson::Value &innerAuthor = innerMsg["author"];
				std::string gname = Utils::Json::getString(innerAuthor, "global_name");
				msg.originalAuthorName = gname.empty() ? Utils::Json::getString(innerAuthor, "username") : gname;
				msg.originalAuthorAvatar = Utils::Json::getString(innerAuthor, "avatar");
			}

			if (innerMsg.HasMember("embeds") && innerMsg["embeds"].IsArray()) {
				const rapidjson::Value &innerEmbeds = innerMsg["embeds"];
				for (rapidjson::SizeType e = 0; e < innerEmbeds.Size(); e++) {
					msg.embeds.push_back(parseEmbedObject(innerEmbeds[e]));
				}
			}

			if (innerMsg.HasMember("attachments") && innerMsg["attachments"].IsArray()) {
				const rapidjson::Value &innerAtts = innerMsg["attachments"];
				for (rapidjson::SizeType a = 0; a < innerAtts.Size(); a++) {
					msg.attachments.push_back(parseAttachmentObject(innerAtts[a]));
				}
			}
		}
	}

	if (d.HasMember("referenced_message") && d["referenced_message"].IsObject()) {
		const rapidjson::Value &refMsg = d["referenced_message"];
		msg.referencedMessageId = Utils::Json::getString(refMsg, "id");
		msg.referencedContent = Utils::Json::getString(refMsg, "content");
		if (refMsg.HasMember("author") && refMsg["author"].IsObject()) {
			const rapidjson::Value &refAuthor = refMsg["author"];
			std::string gname = Utils::Json::getString(refAuthor, "global_name");
			msg.referencedAuthorName = gname.empty() ? Utils::Json::getString(refAuthor, "username") : gname;

			bool hasRefMember = (refMsg.HasMember("member") && refMsg["member"].IsObject());
			Member refMember;
			bool foundMember = false;

			if (hasRefMember) {
				const rapidjson::Value &refMem = refMsg["member"];
				std::string nick = Utils::Json::getString(refMem, "nick");
				if (!nick.empty()) {
					msg.referencedAuthorNickname = nick;
				}

				if (refMem.HasMember("roles") && refMem["roles"].IsArray()) {
					const rapidjson::Value &roles = refMem["roles"];
					Member temp;
					temp.user_id = Utils::Json::getString(refAuthor, "id");
					for (rapidjson::SizeType i = 0; i < roles.Size(); i++) {
						if (roles[i].IsString()) {
							temp.role_ids.push_back(roles[i].GetString());
						}
					}
					std::string guildId = getGuildIdFromChannel(msg.channelId);
					if (!guildId.empty()) {
						msg.referencedAuthorColor = getRoleColor(guildId, temp);
					}
				}
				foundMember = true;
			}

			if (!foundMember) {
				std::string guildId = getGuildIdFromChannel(msg.channelId);
				std::string authorId = Utils::Json::getString(refAuthor, "id");
				if (!guildId.empty() && !authorId.empty()) {
					Member m = getMember(guildId, authorId);
					if (!m.user_id.empty()) {
						if (!m.nickname.empty()) {
							msg.referencedAuthorNickname = m.nickname;
						}
						msg.referencedAuthorColor = m.role_ids.empty() ? 0 : getRoleColor(guildId, m);
					}
				}
			}
		}
	}

	msg.displayContent = UI::MessageUtils::buildDisplayContent(msg);

	for (auto &embed : msg.embeds) {
		if (!embed.description.empty()) {
			embed.description = UI::MessageUtils::formatMentions(embed.description, msg);
		}
		for (auto &field : embed.fields) {
			if (!field.value.empty()) {
				field.value = UI::MessageUtils::formatMentions(field.value, msg);
			}
		}
	}

	return msg;
}

void DiscordClient::sendHeartbeat() {
	char buffer[128];
	uint64_t sequence = lastSequence.load();
	if (sequence != 0) {
		snprintf(buffer, sizeof(buffer), "{\"op\": 1, \"d\": %llu}", (unsigned long long)sequence);
	} else {
		snprintf(buffer, sizeof(buffer), "{\"op\": 1, \"d\": null}");
	}
	queueSend(buffer);
	Logger::log("[Gateway] Sent Heartbeat");
}

// The streamed READY only knows its guild count at the end, too late to show.
void DiscordClient::requestGuildCount() {
	expectedGuildCount.store(-1);
	Network::NetworkManager::getInstance().enqueue(
	    "https://discord.com/api/v10/users/@me/guilds", "GET", "", Network::RequestPriority::INTERACTIVE,
	    [this](const Network::HttpResponse &resp) {
		    if (!resp.success || resp.statusCode != 200) {
			    return;
		    }
		    Network::JsonInputStream stream(resp.body.data(), resp.body.size());
		    JsonPull p(stream);
		    if (!p.next() || p.token() != JsonPull::Token::START_ARRAY) {
			    return;
		    }
		    int count = 0;
		    while (p.skipValue()) {
			    count++;
		    }
		    if (p.ok()) {
			    expectedGuildCount.store(count);
		    }
	    },
	    {{"Authorization", token}});
}

void DiscordClient::sendIdentify() {
	requestGuildCount();

	std::string json = "{"
	                   "\"op\": 2,"
	                   "\"d\": {"
	                   "\"token\": \"" +
	                   token +
	                   "\","
	                   "\"properties\": {"
	                   "\"os\": \"" +
	                   Utils::System::getDeviceModelName() +
	                   "\","
	                   "\"browser\": \"TriCord\","
	                   "\"device\": \"" +
	                   Utils::System::getDeviceModelName() +
	                   "\""
	                   "},"
	                   "\"compress\": false,"
	                   // LAZY_USER_NOTES | VERSIONED_READ_STATES |
	                   // VERSIONED_USER_GUILD_SETTINGS | PRIORITIZED_READY_PAYLOAD
	                   "\"capabilities\": 45,"
	                   "\"large_threshold\": 50"
	                   "}"
	                   "}";
	queueSend(json);
	Logger::log("[Gateway] Sent Identify");
}

void DiscordClient::sendResume() {
	std::string resumeSessionId;
	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		resumeSessionId = sessionId;
	}
	uint64_t sequence = lastSequence.load();
	std::string json = "{"
	                   "\"op\": 6,"
	                   "\"d\": {"
	                   "\"token\": \"" +
	                   token +
	                   "\","
	                   "\"session_id\": \"" +
	                   resumeSessionId +
	                   "\","
	                   "\"seq\": " +
	                   std::to_string(sequence) +
	                   "}"
	                   "}";
	queueSend(json);
	Logger::log("[Gateway] Sent Resume (seq: %llu)", (unsigned long long)sequence);
}

void DiscordClient::handleInvalidSession(const rapidjson::Document &doc) {
	bool resumable = Utils::Json::getBool(doc, "d");
	Logger::log("[Gateway] Invalid Session. Resumable: %d", resumable);
	if (resumable) {
		sendResume();
	} else {
		invalidateSession();
		sendIdentify();
	}
}

void DiscordClient::handleReconnect() {
	Logger::log("[Gateway] Reconnect requested (Op 7)");
	// The socket belongs to the network thread.
	reconnectRequested = true;
}

void DiscordClient::fetchMessagesAsync(const std::string &channelId, int limit, MessagesCallback cb,
                                       const std::string &aroundId) {
	if (channelId.empty() || token.empty()) {
		if (cb) {
			cb({});
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages?limit=" + std::to_string(limit);
	if (!aroundId.empty()) {
		url += "&around=" + aroundId;
	}

	Network::NetworkManager::getInstance().enqueue(
	    url, "GET", "", Network::RequestPriority::INTERACTIVE,
	    [this, cb, channelId](const Network::HttpResponse &resp) {
		    std::vector<Message> messages;
		    if (resp.success && resp.statusCode == 200) {
			    messages = parseMessages(resp.body);
			    if (messages.empty()) {

				    Logger::log("Fetched 0 messages for channel %s. Body len: %zu", channelId.c_str(),
				                resp.body.size());
			    }
		    } else {
			    Logger::log("Failed to fetch messages for %s: Status %d", channelId.c_str(), resp.statusCode);
			    Logger::log("Response body: %s", resp.body.c_str());
		    }
		    if (cb) {
			    cb(messages);
		    }
	    },
	    {{"Authorization", token}});
}

void DiscordClient::fetchMessagesBeforeAsync(const std::string &channelId, const std::string &beforeId, int limit,
                                             MessagesCallback cb) {
	if (channelId.empty() || token.empty() || beforeId.empty()) {
		if (cb) {
			cb({});
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages?limit=" + std::to_string(limit) +
	                  "&before=" + beforeId;

	Network::NetworkManager::getInstance().enqueue(
	    url, "GET", "", Network::RequestPriority::BACKGROUND,
	    [this, cb, channelId](const Network::HttpResponse &resp) {
		    std::vector<Message> messages;
		    if (resp.success && resp.statusCode == 200) {
			    messages = parseMessages(resp.body);
		    } else {
			    Logger::log("Failed to fetch older messages for %s: Status %d", channelId.c_str(), resp.statusCode);
		    }
		    if (cb) {
			    cb(messages);
		    }
	    },
	    {{"Authorization", token}});
}

void DiscordClient::fetchMessage(const std::string &channelId, const std::string &messageId, SingleMessageCallback cb) {
	if (channelId.empty() || messageId.empty() || token.empty()) {
		if (cb) {
			cb(std::nullopt);
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages/" + messageId;

	Network::NetworkManager::getInstance().enqueue(url, "GET", "", Network::RequestPriority::INTERACTIVE,
	                                               [this, cb](const Network::HttpResponse &resp) {
		                                               if (resp.success && resp.statusCode == 200) {
			                                               rapidjson::Document doc;
			                                               doc.Parse(resp.body.c_str());
			                                               if (!doc.HasParseError() && doc.IsObject()) {
				                                               if (cb) {
					                                               cb(parseSingleMessage(doc));
				                                               }
				                                               return;
			                                               }
		                                               }
		                                               if (cb) {
			                                               cb(std::nullopt);
		                                               }
	                                               },
	                                               {{"Authorization", token}});
}

std::vector<Message> DiscordClient::parseMessages(const std::string &json) {
	std::vector<Message> messages;
	rapidjson::Document doc;

	if (json.empty()) {
		return messages;
	}
	std::string buffer = json;
	doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(&buffer[0]);

	if (!doc.HasParseError() && doc.IsArray()) {
		for (rapidjson::SizeType i = 0; i < doc.Size(); i++) {
			messages.push_back(parseSingleMessage(doc[i]));
		}
	}

	return messages;
}

Channel DiscordClient::getChannel(const std::string &channelId) {
	for (const auto &c : privateChannels) {
		if (c.id == channelId) {
			return c;
		}
	}
	for (const auto &g : guilds) {
		for (const auto &c : g.channels) {
			if (c.id == channelId) {
				return c;
			}
		}
	}
	return Channel();
}

const Channel *DiscordClient::getChannelPtr(const std::string &channelId) {
	for (const auto &c : privateChannels) {
		if (c.id == channelId) {
			return &c;
		}
	}
	for (const auto &g : guilds) {
		for (const auto &c : g.channels) {
			if (c.id == channelId) {
				return &c;
			}
		}
	}
	return nullptr;
}

Guild DiscordClient::getGuild(const std::string &guildId) {
	for (const auto &guild : guilds) {
		if (guild.id == guildId) {
			return guild;
		}
	}
	return Guild();
}

const Guild *DiscordClient::getGuildPtr(const std::string &guildId) { return findGuildLocked(guildId); }

Member DiscordClient::getMember(const std::string &guildId, const std::string &userId) {
	for (const auto &guild : guilds) {
		if (guild.id == guildId) {
			for (const auto &member : guild.members) {
				if (member.user_id == userId) {
					return member;
				}
			}
			break;
		}
	}
	return Member();
}

int DiscordClient::getRoleColor(const std::string &guildId, const Member &member) {
	if (member.role_ids.empty()) {
		return 0;
	}

	for (const auto &guild : guilds) {
		if (guild.id == guildId) {
			int highestPos = -1;
			int color = 0;

			for (const auto &roleId : member.role_ids) {
				for (const auto &role : guild.roles) {
					if (role.id == roleId && role.color != 0) {
						if (role.position > highestPos) {
							highestPos = role.position;
							color = role.color;
						}
					}
				}
			}
			return color;
		}
	}
	return 0;
}

int DiscordClient::getRoleColor(const std::string &guildId, const std::string &userId) {
	Member member = getMember(guildId, userId);
	if (!member.user_id.empty()) {
		return getRoleColor(guildId, member);
	}
	return 0;
}

std::string DiscordClient::getMemberDisplayName(const std::string &guildId, const std::string &userId,
                                                const User &user) {
	Member member = getMember(guildId, userId);

	if (!member.nickname.empty()) {
		return member.nickname;
	}
	if (!user.global_name.empty()) {
		return user.global_name;
	}
	return user.username;
}

std::string DiscordClient::getGuildIdFromChannel(const std::string &channelId) {
	if (channelId.empty()) {
		return "";
	}

	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		auto it = channelToGuildCache.find(channelId);
		if (it != channelToGuildCache.end()) {
			return it->second;
		}

		for (const auto &pc : privateChannels) {
			if (pc.id == channelId) {
				channelToGuildCache[channelId] = "DM";
				return "DM";
			}
		}

		for (const auto &guild : guilds) {
			for (const auto &channel : guild.channels) {
				if (channel.id == channelId) {
					channelToGuildCache[channelId] = guild.id;
					return guild.id;
				}
			}
		}
	}
	return "";
}

bool DiscordClient::parseGuildBody(const Network::HttpResponse &resp, Guild &guild) {
	if (!resp.success || resp.statusCode != 200) {
		return false;
	}
	Network::JsonInputStream stream(resp.body.data(), resp.body.size());
	JsonPull p(stream);
	return p.next() && p.token() == JsonPull::Token::START_OBJECT && parseGuildFields(p, guild, "", nullptr, true);
}

void DiscordClient::fetchGuildDetails(const std::string &guildId, std::function<void(bool)> cb) {
	// An unloaded guild gets its summary together with its channels.
	if (token.empty() || guildId.empty() || getGuildChannelsState(guildId) != GuildChannelsState::LOADED) {
		if (cb) {
			cb(false);
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/guilds/" + guildId + "?with_counts=true";

	Network::NetworkManager::getInstance().enqueue(url, "GET", "", Network::RequestPriority::INTERACTIVE,
	                                               [this, guildId, cb](Network::HttpResponse &resp) {
		                                               Guild details;
		                                               bool parsed = parseGuildBody(resp, details);
		                                               if (parsed) {
			                                               std::lock_guard<std::recursive_mutex> lock(clientMutex);
			                                               Guild *guild = findGuildLocked(guildId);
			                                               if (guild) {
				                                               applyGuildDetailsLocked(*guild, details);
			                                               }
		                                               }
		                                               if (cb) {
			                                               cb(parsed);
		                                               }
	                                               },
	                                               {{"Authorization", token}});
}

UserProfile DiscordClient::getUserProfile(const std::string &userId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	auto it = profileCache.find(userId);
	if (it != profileCache.end()) {
		return it->second;
	}
	return UserProfile();
}

void DiscordClient::fetchUserProfile(const std::string &userId) {
	if (token.empty() || userId.empty()) {
		return;
	}
	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		if (profileCache.count(userId)) {
			return;
		}
		profileCache[userId].userId = userId;
	}

	std::string url = "https://discord.com/api/v10/users/" + userId + "/profile?with_mutual_guilds=false";
	Network::NetworkManager::getInstance().enqueue(url, "GET", "", Network::RequestPriority::BACKGROUND,
	                                               [this, userId](const Network::HttpResponse &resp) {
		                                               if (!resp.success) {
			                                               return;
		                                               }
		                                               rapidjson::Document doc;
		                                               doc.Parse(resp.body.c_str());
		                                               if (doc.HasParseError() || !doc.IsObject()) {
			                                               return;
		                                               }

		                                               UserProfile profile;
		                                               profile.userId = userId;
		                                               profile.loaded = true;
		                                               if (doc.HasMember("user") && doc["user"].IsObject()) {
			                                               profile.bio = Utils::Json::getString(doc["user"], "bio");
		                                               }
		                                               if (doc.HasMember("user_profile") &&
		                                                   doc["user_profile"].IsObject()) {
			                                               const rapidjson::Value &up = doc["user_profile"];
			                                               profile.pronouns = Utils::Json::getString(up, "pronouns");
			                                               if (profile.bio.empty()) {
				                                               profile.bio = Utils::Json::getString(up, "bio");
			                                               }
		                                               }

		                                               std::lock_guard<std::recursive_mutex> lock(clientMutex);
		                                               profileCache[userId] = profile;
	                                               },
	                                               {{"Authorization", token}});
}

Message DiscordClient::parseSingleMessage(const std::string &json) {
	rapidjson::Document doc;
	std::string buffer = json;
	doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(&buffer[0]);

	if (doc.HasParseError() || !doc.IsObject()) {
		return Message();
	}

	return parseSingleMessage(doc);
}

uint64_t DiscordClient::calcBasePermissions(const Guild &guild, const std::string &userId,
                                            const std::vector<std::string> &memberRoleIds) {

	if (!userId.empty() && userId == guild.ownerId) {
		return ~0ULL;
	}

	uint64_t permissions = 0;

	for (const auto &role : guild.roles) {
		if (role.id == guild.id) {
			permissions |= role.permissions;
			break;
		}
	}

	for (const auto &roleId : memberRoleIds) {
		for (const auto &role : guild.roles) {
			if (role.id == roleId) {
				permissions |= role.permissions;
				break;
			}
		}
	}

	if (permissions & Permissions::ADMINISTRATOR) {
		return ~0ULL;
	}

	return permissions;
}

uint64_t DiscordClient::applyChannelOverwrites(uint64_t base, const Guild &guild, const Channel *parent,
                                               const Channel &channel, const std::string &userId,
                                               const std::vector<std::string> &memberRoleIds) {
	if (base & Permissions::ADMINISTRATOR) {
		return ~0ULL;
	}

	uint64_t perms = base;
	if (parent) {
		perms = computeOverwrites(perms, guild.id, userId, memberRoleIds, parent->permission_overwrites);
	}
	return computeOverwrites(perms, guild.id, userId, memberRoleIds, channel.permission_overwrites);
}

uint64_t DiscordClient::computeChannelPermissions(const Guild &guild, const Channel &channel, const std::string &userId,
                                                  const std::vector<std::string> &memberRoleIds) {
	const Channel *parent = nullptr;
	if (!channel.parent_id.empty()) {
		for (const auto &candidate : guild.channels) {
			if (candidate.id == channel.parent_id) {
				parent = &candidate;
				break;
			}
		}
	}
	return applyChannelOverwrites(calcBasePermissions(guild, userId, memberRoleIds), guild, parent, channel, userId,
	                              memberRoleIds);
}

uint64_t DiscordClient::computeOverwrites(uint64_t basePermissions, const std::string &guildId,
                                          const std::string &memberId, const std::vector<std::string> &memberRoleIds,
                                          const rapidjson::Value &channelObj) {

	std::vector<Overwrite> overwrites;

	if (channelObj.HasMember("permission_overwrites") && channelObj["permission_overwrites"].IsArray()) {
		const rapidjson::Value &ows = channelObj["permission_overwrites"];
		for (rapidjson::SizeType i = 0; i < ows.Size(); i++) {
			const rapidjson::Value &ow = ows[i];
			Overwrite o;
			o.id = Utils::Json::getString(ow, "id");
			o.type = Utils::Json::getInt(ow, "type");
			o.allow = Utils::Json::getUint64(ow, "allow");
			o.deny = Utils::Json::getUint64(ow, "deny");

			overwrites.push_back(o);
		}
	}

	return computeOverwrites(basePermissions, guildId, memberId, memberRoleIds, overwrites);
}

uint64_t DiscordClient::computeOverwrites(uint64_t basePermissions, const std::string &guildId,
                                          const std::string &memberId, const std::vector<std::string> &memberRoleIds,
                                          const std::vector<Overwrite> &overwrites) {

	if (basePermissions & Permissions::ADMINISTRATOR) {
		return ~0ULL;
	}

	uint64_t permissions = basePermissions;
	uint64_t everyoneAllow = 0, everyoneDeny = 0;
	uint64_t roleAllow = 0, roleDeny = 0;
	uint64_t memberAllow = 0, memberDeny = 0;
	bool hasMemberOverwrite = false;

	for (const auto &ow : overwrites) {
		if (ow.type == 0) {
			if (ow.id == guildId) {
				everyoneAllow = ow.allow;
				everyoneDeny = ow.deny;
			} else {
				for (const auto &rId : memberRoleIds) {
					if (rId == ow.id) {
						roleAllow |= ow.allow;
						roleDeny |= ow.deny;
						break;
					}
				}
			}
		} else if (ow.type == 1) {
			if (ow.id == memberId) {
				memberAllow = ow.allow;
				memberDeny = ow.deny;
				hasMemberOverwrite = true;
			}
		}
	}

	permissions &= ~everyoneDeny;
	permissions |= everyoneAllow;
	permissions &= ~roleDeny;
	permissions |= roleAllow;
	if (hasMemberOverwrite) {
		permissions &= ~memberDeny;
		permissions |= memberAllow;
	}

	return permissions;
}

void DiscordClient::sendMessage(const std::string &channelId, const std::string &content, SendMessageCallback cb,
                                const std::string &nonce) {
	postMessage(channelId, content, nonce, "", cb);
}

void DiscordClient::sendReply(const std::string &channelId, const std::string &content, const std::string &replyId,
                              SendMessageCallback cb, const std::string &nonce) {
	postMessage(channelId, content, nonce, replyId, cb);
}

void DiscordClient::postMessage(const std::string &channelId, const std::string &content, const std::string &nonce,
                                const std::string &replyId, SendMessageCallback cb) {
	if (token.empty() || channelId.empty() || content.empty()) {
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages";

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("content");
	writer.String(content.c_str());

	writer.Key("nonce");
	if (!nonce.empty()) {
		writer.String(nonce.c_str());
	} else {
		writer.String(std::to_string(osGetTime()).c_str());
	}

	if (!replyId.empty()) {
		writer.Key("message_reference");
		writer.StartObject();
		writer.Key("message_id");
		writer.String(replyId.c_str());
		writer.EndObject();
	}

	writer.Key("tts");
	writer.Bool(false);
	writer.Key("flags");
	writer.Int(0);
	writer.EndObject();

	Network::NetworkManager::getInstance().enqueue(
	    url, "POST", s.GetString(), Network::RequestPriority::REALTIME,
	    [this, cb](const Network::HttpResponse &resp) {
		    if (!resp.success || resp.statusCode >= 400) {
			    Logger::log("Failed to post message: %d (%s)", (int)resp.statusCode, resp.error.c_str());
			    if (cb) {
				    cb(Message(), false, (int)resp.statusCode);
			    }
			    return;
		    }
		    if (cb) {
			    cb(parseSingleMessage(resp.body), true, (int)resp.statusCode);
		    }
	    },
	    {{"Authorization", token},
	     {"Content-Type", "application/json"},
	     {"X-Context-Properties", "eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="}});
}

void DiscordClient::sendMessageAsync(const std::string &channelId, const std::string &content, SuccessCallback cb) {
	if (token.empty() || channelId.empty() || content.empty()) {
		if (cb) {
			cb(false);
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages";

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("content");
	writer.String(content.c_str());
	writer.Key("flags");
	writer.Int(0);
	writer.Key("nonce");
	writer.String(std::to_string(osGetTime()).c_str());
	writer.Key("tts");
	writer.Bool(false);
	writer.EndObject();

	Network::NetworkManager::getInstance().enqueue(
	    url, "POST", s.GetString(), Network::RequestPriority::REALTIME,
	    [cb](const Network::HttpResponse &resp) {
		    if (cb) {
			    cb(resp.success);
		    }
	    },
	    {{"Authorization", token}, {"X-Context-Properties", "eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="}});
}

bool DiscordClient::editMessage(const std::string &channelId, const std::string &messageId,
                                const std::string &content) {
	if (token.empty() || channelId.empty() || messageId.empty() || content.empty()) {
		return false;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages/" + messageId;

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("content");
	writer.String(content.c_str());
	writer.EndObject();

	Network::HttpClient http;
	http.setAuthToken(token);

	Network::HttpResponse resp = http.patch(url, s.GetString());
	return resp.success;
}

void DiscordClient::editMessageAsync(const std::string &channelId, const std::string &messageId,
                                     const std::string &content, SuccessCallback cb) {
	if (token.empty() || channelId.empty() || messageId.empty() || content.empty()) {
		if (cb) {
			cb(false);
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages/" + messageId;

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("content");
	writer.String(content.c_str());
	writer.EndObject();

	Network::NetworkManager::getInstance().enqueue(url, "PATCH", s.GetString(), Network::RequestPriority::INTERACTIVE,
	                                               [cb](const Network::HttpResponse &resp) {
		                                               if (cb) {
			                                               cb(resp.success);
		                                               }
	                                               },
	                                               {{"Authorization", token}});
}

struct ThreadFetchContext {
	std::vector<Channel> threads;
	struct OPInfo {
		std::string content;
		std::string authorId;
		std::string authorName;
		int authorColor = 0;
	};
	std::map<std::string, OPInfo> opInfos;
	int remaining = 2;
	std::mutex mutex;
};

void DiscordClient::fetchForumThreads(const std::string &channelId, ThreadsCallback cb) {
	if (token.empty() || channelId.empty()) {
		if (cb) {
			cb({});
		}
		return;
	}

	auto ctx = std::make_shared<ThreadFetchContext>();

	auto performFetch = [this, channelId, cb, ctx](bool archived) {
		std::string url = "https://discord.com/api/v10/channels/" + channelId +
		                  "/threads/search?archived=" + (archived ? "true" : "false") +
		                  "&sort_by=last_message_time&sort_order=desc&limit=25&"
		                  "offset=0";

		Network::NetworkManager::getInstance().enqueue(
		    url, "GET", "", Network::RequestPriority::INTERACTIVE,
		    [this, channelId, cb, ctx](const Network::HttpResponse &resp) {
			    {
				    std::lock_guard<std::mutex> lock(ctx->mutex);
				    ctx->remaining--;

				    if (resp.success) {
					    rapidjson::Document doc;
					    doc.Parse(resp.body.c_str());

					    if (!doc.HasParseError() && doc.IsObject()) {
						    std::string guildId = getGuildIdFromChannel(channelId);

						    if (doc.HasMember("threads") && doc["threads"].IsArray()) {
							    const rapidjson::Value &threadArray = doc["threads"];
							    for (rapidjson::SizeType i = 0; i < threadArray.Size(); i++) {
								    const rapidjson::Value &tObj = threadArray[i];
								    Channel t;
								    parseChannelObject(tObj, t);
								    if (!tObj.HasMember("type")) {
									    t.type = 11;
								    }
								    t.message_count = Utils::Json::getInt(tObj, "message_count");
								    t.owner_id = Utils::Json::getString(tObj, "owner_id");
								    t.viewable = true;
								    t.is_archived = false;
								    if (tObj.HasMember("thread_metadata") && tObj["thread_metadata"].IsObject()) {
									    t.is_archived = Utils::Json::getBool(tObj["thread_metadata"], "archived");
								    }

								    ctx->threads.push_back(t);
							    }
						    }

						    if (doc.HasMember("first_messages") && doc["first_messages"].IsArray()) {
							    const rapidjson::Value &msgs = doc["first_messages"];
							    for (rapidjson::SizeType i = 0; i < msgs.Size(); i++) {
								    const rapidjson::Value &mObj = msgs[i];
								    Message msg = parseSingleMessage(mObj);

								    ThreadFetchContext::OPInfo info;
								    info.content = msg.content;
								    info.authorId = msg.author.id;

								    if (info.content.empty()) {
									    if (mObj.HasMember("attachments") && mObj["attachments"].IsArray() &&
									        mObj["attachments"].Size() > 0) {
										    info.content = "[Image]";
									    } else if (mObj.HasMember("embeds") && mObj["embeds"].IsArray() &&
									               mObj["embeds"].Size() > 0) {
										    info.content = "[Embed]";
									    }
								    }

								    if (!msg.author.id.empty()) {
									    if (!msg.member.nickname.empty()) {
										    info.authorName = msg.member.nickname;
									    } else {
										    info.authorName = getMemberDisplayName(guildId, msg.author.id, msg.author);
									    }

									    info.authorColor = getRoleColor(guildId, msg.member);
									    if (info.authorColor == 0) {
										    info.authorColor = getRoleColor(guildId, msg.author.id);
									    }
								    }

								    if (!msg.channelId.empty()) {
									    ctx->opInfos[msg.channelId] = info;
								    }
							    }
						    }
					    }
				    }
			    }

			    if (ctx->remaining == 0) {

				    for (auto &t : ctx->threads) {
					    if (ctx->opInfos.count(t.id)) {
						    const auto &info = ctx->opInfos[t.id];
						    t.op_content = info.content;
						    t.owner_id = info.authorId;
						    t.owner_name = info.authorName;
						    t.owner_color = info.authorColor;
					    }
				    }

				    std::string guildId = getGuildIdFromChannel(channelId);
				    if (!guildId.empty()) {
					    std::lock_guard<std::recursive_mutex> lock(clientMutex);
					    Guild *guild = findGuildLocked(guildId);
					    if (guild && guild->channelsLoaded) {
						    for (const auto &t : ctx->threads) {
							    bool exists = false;
							    for (const auto &existing : guild->channels) {
								    if (existing.id == t.id) {
									    exists = true;
									    break;
								    }
							    }
							    if (!exists) {
								    guild->channels.push_back(t);
							    }
						    }
					    }
				    }

				    if (cb) {
					    cb(ctx->threads);
				    }
			    }
		    },
		    {{"Authorization", token}});
	};

	performFetch(false);
	performFetch(true);
}

void DiscordClient::hasArchivedThreads(const std::string &channelId, std::function<void(bool)> cb) {
	if (token.empty() || channelId.empty()) {
		if (cb) {
			cb(false);
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId +
	                  "/threads/search?archived=true&sort_by=last_message_time&sort_order=desc&limit=1&offset=0";

	Network::NetworkManager::getInstance().enqueue(url, "GET", "", Network::RequestPriority::INTERACTIVE,
	                                               [cb](const Network::HttpResponse &resp) {
		                                               bool found = false;
		                                               if (resp.success) {
			                                               rapidjson::Document doc;
			                                               doc.Parse(resp.body.c_str());
			                                               found = !doc.HasParseError() && doc.IsObject() &&
			                                                       doc.HasMember("threads") &&
			                                                       doc["threads"].IsArray() && !doc["threads"].Empty();
		                                               }
		                                               if (cb) {
			                                               cb(found);
		                                               }
	                                               },
	                                               {{"Authorization", token}});
}

bool DiscordClient::deleteMessage(const std::string &channelId, const std::string &messageId) {
	if (token.empty() || channelId.empty() || messageId.empty()) {
		return false;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages/" + messageId;

	Network::HttpClient http;
	http.setAuthToken(token);

	Network::HttpResponse resp = http.del(url);
	return resp.success;
}

void DiscordClient::deleteMessageAsync(const std::string &channelId, const std::string &messageId, SuccessCallback cb) {
	if (token.empty() || channelId.empty() || messageId.empty()) {
		if (cb) {
			cb(false);
		}
		return;
	}

	std::string url = "https://discord.com/api/v10/channels/" + channelId + "/messages/" + messageId;

	Network::NetworkManager::getInstance().enqueue(url, "DELETE", "", Network::RequestPriority::REALTIME,
	                                               [cb](const Network::HttpResponse &resp) {
		                                               if (cb) {
			                                               cb(resp.success);
		                                               }
	                                               },
	                                               {{"Authorization", token}});
}

void DiscordClient::exchangeTicketForToken(const std::string &ticket, TokenCallback cb) {
	Logger::log("[DiscordClient] Exchanging ticket for token");

	std::string url = "https://discord.com/api/v10/users/@me/remote-auth/login";

	rapidjson::Document doc;
	doc.SetObject();
	rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();

	doc.AddMember("ticket", rapidjson::Value(ticket.c_str(), allocator), allocator);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);
	std::string payload = buffer.GetString();

	Logger::log("[DiscordClient] Exchange payload prepared (len %zu)", payload.size());

	Network::NetworkManager::getInstance().enqueue(
	    url, "POST", payload, Network::RequestPriority::INTERACTIVE,
	    [cb](const Network::HttpResponse &resp) {
		    if (!resp.success) {
			    Logger::log("[DiscordClient] Token exchange failed: %d", resp.statusCode);
			    Logger::log("[DiscordClient] Response body: %s", resp.body.c_str());
			    if (cb) {
				    cb("");
			    }
			    return;
		    }

		    rapidjson::Document doc;
		    doc.Parse(resp.body.c_str());

		    if (doc.HasParseError() || !doc.IsObject()) {
			    Logger::log("[DiscordClient] Failed to parse token response");
			    if (cb) {
				    cb("");
			    }
			    return;
		    }

		    std::string token = Utils::Json::getString(doc, "encrypted_token");
		    Logger::log("[DiscordClient] Token received (len %zu)", token.size());

		    if (cb) {
			    cb(token);
		    }
	    },
	    {{"Content-Type", "application/json"}});
}

void DiscordClient::requestMembers(const std::string &guildId, const std::vector<std::string> &userIds) {
	if (guildId.empty() || userIds.empty() || guildId == "DM") {
		return;
	}

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(8);
	writer.Key("d");
	writer.StartObject();
	writer.Key("guild_id");
	writer.String(guildId.c_str());
	writer.Key("user_ids");
	writer.StartArray();
	// The gateway caps a request at 100 IDs.
	for (size_t i = 0; i < userIds.size() && i < 100; i++) {
		writer.String(userIds[i].c_str());
	}
	writer.EndArray();
	writer.EndObject();
	writer.EndObject();

	queueSend(s.GetString());
	Logger::log("[Gateway] Requested %zu members for guild %s", userIds.size(), guildId.c_str());
}

void DiscordClient::applyVoiceMemberName(const std::string &userId, const Member &member) {
	voiceNameLookups.erase(userId);

	std::string name =
	    !member.nickname.empty() ? member.nickname : (!member.globalName.empty() ? member.globalName : member.username);
	if (name.empty()) {
		return;
	}

	auto it = voiceChannelByUser.find(userId);
	if (it == voiceChannelByUser.end()) {
		return;
	}
	for (auto &p : voiceParticipants[it->second]) {
		if (p.userId == userId) {
			p.name = name;
			p.avatar = member.avatar;
			AvatarCache::getInstance().prefetchAvatar(userId, p.avatar, "0");
			break;
		}
	}
}

void DiscordClient::handleGuildMembersChunk(const rapidjson::Value &d) {
	std::string guildId = Utils::Json::getString(d, "guild_id");
	if (guildId.empty() || !d.HasMember("members") || !d["members"].IsArray()) {
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	for (auto &g : guilds) {
		if (g.id != guildId) {
			continue;
		}

		for (const auto &mObj : d["members"].GetArray()) {
			if (!mObj.IsObject() || !mObj.HasMember("user") || !mObj["user"].IsObject()) {
				continue;
			}
			std::string uid = Utils::Json::getString(mObj["user"], "id");
			if (uid.empty()) {
				continue;
			}

			Member member = parseMemberObject(mObj, uid);
			bool found = false;
			for (auto &m : g.members) {
				if (m.user_id == uid) {
					m = member;
					found = true;
					break;
				}
			}
			if (!found) {
				g.members.push_back(member);
			}

			applyVoiceMemberName(uid, member);
		}
		break;
	}

	guildDataDirty.store(true);
}

void DiscordClient::performLogin(const std::string &email, const std::string &password, LoginCallback cb) {
	rapidjson::Document d;
	d.SetObject();
	rapidjson::Document::AllocatorType &allocator = d.GetAllocator();

	d.AddMember("login", rapidjson::Value(email.c_str(), allocator), allocator);
	d.AddMember("password", rapidjson::Value(password.c_str(), allocator), allocator);
	d.AddMember("undelete", false, allocator);
	d.AddMember("captcha_key", rapidjson::Value(), allocator);
	d.AddMember("login_source", rapidjson::Value(), allocator);
	d.AddMember("gift_code_sku_id", rapidjson::Value(), allocator);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	std::string json = buffer.GetString();

	Network::NetworkManager::getInstance().enqueue(
	    "https://discord.com/api/v10/auth/login", "POST", json, Network::RequestPriority::INTERACTIVE,
	    [cb](const Network::HttpResponse &resp) {
		    if (resp.statusCode == 200) {
			    rapidjson::Document doc;
			    doc.Parse(resp.body.c_str());

			    if (doc.HasParseError()) {
				    if (cb) {
					    cb(false, "", false, "", "Response parse error");
				    }
				    return;
			    }

			    if (doc.HasMember("mfa") && doc["mfa"].IsBool() && doc["mfa"].GetBool()) {
				    std::string ticket = "";
				    if (doc.HasMember("ticket") && doc["ticket"].IsString()) {
					    ticket = doc["ticket"].GetString();
				    }
				    if (cb) {
					    cb(false, "", true, ticket, "");
				    }
			    } else if (doc.HasMember("token") && doc["token"].IsString()) {
				    std::string token = doc["token"].GetString();
				    if (cb) {
					    cb(true, token, false, "", "");
				    }
			    } else {
				    if (cb) {
					    cb(false, "", false, "", "Unknown response format");
				    }
			    }
		    } else {
			    std::string error = "Login failed: " + std::to_string(resp.statusCode);
			    rapidjson::Document doc;
			    doc.Parse(resp.body.c_str());
			    if (!doc.HasParseError() && doc.IsObject()) {
				    if (doc.HasMember("captcha_key") && doc["captcha_key"].IsArray()) {
					    error = "CAPTCHA required. Please use QR code login.";
				    } else if (doc.HasMember("errors") && doc["errors"].IsObject()) {
					    const rapidjson::Value &errors = doc["errors"];
					    bool found = false;
					    for (auto it = errors.MemberBegin(); it != errors.MemberEnd() && !found; ++it) {
						    if (it->value.IsObject() && it->value.HasMember("_errors") &&
						        it->value["_errors"].IsArray() && it->value["_errors"].Size() > 0) {
							    const rapidjson::Value &firstErr = it->value["_errors"][0];
							    if (firstErr.IsObject() && firstErr.HasMember("message") &&
							        firstErr["message"].IsString()) {
								    error = firstErr["message"].GetString();
								    found = true;
							    }
						    }
					    }
					    if (!found && doc.HasMember("message") && doc["message"].IsString()) {
						    error = doc["message"].GetString();
					    }
				    } else if (doc.HasMember("message") && doc["message"].IsString()) {
					    error = doc["message"].GetString();
				    }
			    }
			    if (cb) {
				    cb(false, "", false, "", error);
			    }
		    }
	    },
	    {{"Content-Type", "application/json"}});
}

void DiscordClient::submitMFA(const std::string &ticket, const std::string &code, LoginCallback cb) {
	rapidjson::Document d;
	d.SetObject();
	rapidjson::Document::AllocatorType &allocator = d.GetAllocator();

	d.AddMember("code", rapidjson::Value(code.c_str(), allocator), allocator);
	d.AddMember("ticket", rapidjson::Value(ticket.c_str(), allocator), allocator);
	d.AddMember("login_source", rapidjson::Value(), allocator);
	d.AddMember("gift_code_sku_id", rapidjson::Value(), allocator);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	d.Accept(writer);
	std::string json = buffer.GetString();

	Network::NetworkManager::getInstance().enqueue(
	    "https://discord.com/api/v10/auth/mfa/totp", "POST", json, Network::RequestPriority::INTERACTIVE,
	    [cb](const Network::HttpResponse &resp) {
		    if (resp.statusCode == 200) {
			    rapidjson::Document doc;
			    doc.Parse(resp.body.c_str());
			    if (doc.HasMember("token") && doc["token"].IsString()) {
				    std::string token = doc["token"].GetString();
				    if (cb) {
					    cb(true, token, false, "", "");
				    }
			    } else {
				    if (cb) {
					    cb(false, "", false, "", "No token in MFA response");
				    }
			    }
		    } else {
			    std::string error = "MFA failed: " + std::to_string(resp.statusCode);
			    rapidjson::Document doc;
			    doc.Parse(resp.body.c_str());
			    if (!doc.HasParseError() && doc.HasMember("message") && doc["message"].IsString()) {
				    error = doc["message"].GetString();
			    }
			    if (cb) {
				    cb(false, "", false, "", error);
			    }
		    }
	    },
	    {{"Content-Type", "application/json"}});
}

void DiscordClient::sendLazyRequest(const std::string &guildId, const std::string &channelId) {
	if (guildId.empty() || channelId.empty() || guildId == "DM") {
		return;
	}

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(14);
	writer.Key("d");
	writer.StartObject();

	writer.Key("guild_id");
	writer.String(guildId.c_str());

	writer.Key("typing");
	writer.Bool(true);
	writer.Key("threads");
	writer.Bool(true);
	writer.Key("activities");
	writer.Bool(true);

	writer.Key("members");
	writer.StartArray();
	writer.EndArray();

	writer.Key("channels");
	writer.StartObject();
	writer.Key(channelId.c_str());
	writer.StartArray();
	writer.StartArray();
	writer.Int(0);
	writer.Int(99);
	writer.EndArray();
	writer.EndArray();
	writer.EndObject();

	writer.EndObject();
	writer.EndObject();

	std::string json = s.GetString();
	queueSend(json);
	Logger::log("[Gateway] Sent Lazy Request (Op 14) for Guild %s Channel %s", guildId.c_str(), channelId.c_str());
}

void DiscordClient::clearVoiceParticipant(const std::string &userId) {
	auto prev = voiceChannelByUser.find(userId);
	if (prev == voiceChannelByUser.end()) {
		return;
	}

	auto &list = voiceParticipants[prev->second];
	for (auto it = list.begin(); it != list.end(); ++it) {
		if (it->userId == userId) {
			list.erase(it);
			break;
		}
	}
	if (list.empty()) {
		voiceParticipants.erase(prev->second);
	}
	voiceChannelByUser.erase(prev);
}

void DiscordClient::applyVoiceState(const rapidjson::Value &state, const std::string &guildId) {
	std::string uid = Utils::Json::getString(state, "user_id");
	if (uid.empty()) {
		return;
	}

	clearVoiceParticipant(uid);

	if (!state.HasMember("channel_id") || !state["channel_id"].IsString()) {
		return;
	}
	std::string channelId = state["channel_id"].GetString();

	VoiceParticipant p;
	p.userId = uid;
	p.guildId = guildId;
	p.selfMute = Utils::Json::getBool(state, "self_mute");
	p.selfDeaf = Utils::Json::getBool(state, "self_deaf");
	p.mute = Utils::Json::getBool(state, "mute");
	p.deaf = Utils::Json::getBool(state, "deaf");

	// The gateway guild object omits `member`, so a state seen at GUILD_CREATE
	// only carries the user ID and has to be resolved against the member list.
	if (state.HasMember("member") && state["member"].IsObject()) {
		const rapidjson::Value &member = state["member"];
		p.name = Utils::Json::getString(member, "nick");
		if (member.HasMember("user") && member["user"].IsObject()) {
			const rapidjson::Value &user = member["user"];
			p.avatar = Utils::Json::getString(user, "avatar");
			if (p.name.empty()) {
				p.name = Utils::Json::getString(user, "global_name");
			}
			if (p.name.empty()) {
				p.name = Utils::Json::getString(user, "username");
			}
		}
	} else if (!guildId.empty()) {
		for (const auto &g : guilds) {
			if (g.id != guildId) {
				continue;
			}
			for (const auto &m : g.members) {
				if (m.user_id != uid) {
					continue;
				}
				p.name = !m.nickname.empty() ? m.nickname : (!m.globalName.empty() ? m.globalName : m.username);
				p.avatar = m.avatar;
				break;
			}
			break;
		}
	}

	if (p.name.empty()) {
		if (uid == currentUser.id) {
			p.name = currentUser.global_name.empty() ? currentUser.username : currentUser.global_name;
			p.avatar = currentUser.avatar;
		} else {
			for (const auto &pc : privateChannels) {
				if (pc.id != channelId) {
					continue;
				}
				for (const auto &r : pc.recipients) {
					if (r.id == uid) {
						p.name = r.global_name.empty() ? r.username : r.global_name;
						p.avatar = r.avatar;
						break;
					}
				}
				break;
			}
		}
	}

	// resolveVoiceNames() replaces this once the user looks at that guild.
	if (p.name.empty()) {
		p.name = uid;
	}

	voiceParticipants[channelId].push_back(std::move(p));
	voiceChannelByUser[uid] = channelId;
}

void DiscordClient::resolveVoiceNames(const std::string &guildId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	if (guildId.empty()) {
		return;
	}

	std::vector<std::string> unresolved;
	for (const auto &entry : voiceParticipants) {
		for (const auto &p : entry.second) {
			if (p.guildId != guildId) {
				continue;
			}
			if (p.name != p.userId) {
				AvatarCache::getInstance().prefetchAvatar(p.userId, p.avatar, "0");
			} else if (voiceNameLookups.insert(p.userId).second) {
				unresolved.push_back(p.userId);
			}
		}
	}

	requestMembers(guildId, unresolved);
}

std::vector<VoiceParticipant> DiscordClient::getVoiceParticipants(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	auto it = voiceParticipants.find(channelId);
	if (it == voiceParticipants.end()) {
		return {};
	}
	return it->second;
}

size_t DiscordClient::getVoiceParticipantCount(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	auto it = voiceParticipants.find(channelId);
	if (it == voiceParticipants.end()) {
		return 0;
	}
	return it->second.size();
}

void DiscordClient::updateVoiceState(const std::string &guildId, const std::string &channelId, bool selfMute,
                                     bool selfDeaf) {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(4);
	writer.Key("d");
	writer.StartObject();

	writer.Key("guild_id");
	if (guildId.empty() || guildId == "DM") {
		writer.Null();
	} else {
		writer.String(guildId.c_str());
	}

	writer.Key("channel_id");
	if (channelId.empty()) {
		writer.Null();
	} else {
		writer.String(channelId.c_str());
	}

	writer.Key("self_mute");
	writer.Bool(selfMute);
	writer.Key("self_deaf");
	writer.Bool(selfDeaf);
	writer.Key("self_video");
	writer.Bool(false);

	writer.EndObject();
	writer.EndObject();

	queueSend(s.GetString());
	Logger::log("[Voice] Sent Update Voice State (Op 4) guild=%s channel=%s", guildId.c_str(), channelId.c_str());
}

void DiscordClient::updatePresence(UserStatus status) {
	std::string statusStr = statusToString(status);

	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);
	writer.StartObject();
	writer.Key("op");
	writer.Int(3);
	writer.Key("d");
	writer.StartObject();
	writer.Key("since");
	if (status == UserStatus::IDLE) {
		writer.Uint64((uint64_t)time(NULL) * 1000);
	} else {
		writer.Int(0);
	}
	writer.Key("activities");
	writer.StartArray();
	writer.EndArray();
	writer.Key("status");
	writer.String(statusStr.c_str());
	writer.Key("afk");
	writer.Bool(false);
	writer.EndObject();
	writer.EndObject();

	queueSend(s.GetString());

	if (!token.empty()) {
		std::string url = "https://discord.com/api/v10/users/@me/settings";
		std::string body = "{\"status\":\"" + statusStr + "\"}";

		Network::NetworkManager::getInstance().enqueue(
		    url, "PATCH", body, Network::RequestPriority::INTERACTIVE,
		    [statusStr](const Network::HttpResponse &resp) {
			    if (resp.success) {
				    Logger::log("[API] Successfully updated global status to %s", statusStr.c_str());
			    } else {
				    Logger::log("[API] Failed to update global status: %d %s", resp.statusCode, resp.error.c_str());
			    }
		    },
		    {{"Authorization", token}});
	}

	std::lock_guard<std::recursive_mutex> lock(clientMutex);
	currentUser.status = status;
}

bool DiscordClient::isUserMentioned(const Message &msg) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);

	for (const auto &user : msg.mentions) {
		if (user.id == currentUser.id) {
			return true;
		}
	}

	std::string guildId = getGuildIdFromChannel(msg.channelId);
	if (guildId.empty() || guildId == "DM") {
		return false;
	}

	const GuildNotificationSettings *gs = nullptr;
	auto gsIt = notificationSettings.find(guildId);
	if (gsIt != notificationSettings.end()) {
		gs = &gsIt->second;
	}

	if (!(gs && gs->suppressEveryone) && msg.mentionEveryone) {
		return true;
	}

	if (!(gs && gs->suppressRoles) && !msg.mentionRoles.empty()) {
		for (const auto &guild : guilds) {
			if (guild.id == guildId) {
				for (const auto &roleId : msg.mentionRoles) {
					for (const auto &myRole : guild.myRoles) {
						if (myRole == roleId) {
							return true;
						}
					}
				}
				break;
			}
		}
	}

	return false;
}

bool DiscordClient::canSendMessage(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);

	std::string guildId = getGuildIdFromChannel(channelId);
	if (guildId == "DM") {
		return true;
	}
	if (guildId.empty()) {

		return false;
	}

	for (const auto &guild : guilds) {
		if (guild.id == guildId) {

			for (const auto &channel : guild.channels) {
				if (channel.id == channelId) {
					uint64_t perms = computeChannelPermissions(guild, channel, currentUser.id, guild.myRoles);

					bool canSend =
					    (perms & Permissions::SEND_MESSAGES) != 0 || (perms & Permissions::ADMINISTRATOR) != 0;

					return canSend;
				}
			}

			return false;
		}
	}

	return false;
}

bool DiscordClient::canManageMessages(const std::string &channelId) {
	std::lock_guard<std::recursive_mutex> lock(clientMutex);

	std::string guildId = getGuildIdFromChannel(channelId);
	if (guildId == "DM") {
		return false;
	}
	if (guildId.empty()) {
		return false;
	}

	for (const auto &guild : guilds) {
		if (guild.id == guildId) {
			for (const auto &channel : guild.channels) {
				if (channel.id == channelId) {
					uint64_t perms = computeChannelPermissions(guild, channel, currentUser.id, guild.myRoles);
					return (perms & Permissions::MANAGE_MESSAGES) != 0 || (perms & Permissions::ADMINISTRATOR) != 0;
				}
			}
			return false;
		}
	}

	return false;
}

void DiscordClient::parseChannelObject(const rapidjson::Value &cObj, Channel &channel) {
	channel.id = Utils::Json::getString(cObj, "id");
	channel.name = Utils::Json::getString(cObj, "name");
	channel.type = Utils::Json::getInt(cObj, "type");
	channel.last_message_id = Utils::Json::getString(cObj, "last_message_id");
	channel.parent_id = Utils::Json::getString(cObj, "parent_id");
	channel.position = Utils::Json::getInt(cObj, "position");
	channel.topic = Utils::Json::getString(cObj, "topic");
	channel.flags = Utils::Json::getInt(cObj, "flags");

	if (cObj.HasMember("permission_overwrites") && cObj["permission_overwrites"].IsArray()) {
		parseOverwrites(cObj["permission_overwrites"], channel.permission_overwrites);
	}

	if (cObj.HasMember("recipients") && cObj["recipients"].IsArray()) {
		const rapidjson::Value &recipients = cObj["recipients"];
		std::string generatedName;
		for (rapidjson::SizeType r = 0; r < recipients.Size(); r++) {
			User u = parseUserObject(recipients[r]);
			if (!generatedName.empty()) {
				generatedName += ", ";
			}
			generatedName += u.global_name.empty() ? u.username : u.global_name;
			channel.recipients.push_back(std::move(u));
		}

		if (channel.name.empty()) {
			channel.name = generatedName;
		}
	}

	if (cObj.HasMember("name") && cObj["name"].IsString()) {
		channel.name = Utils::Json::getString(cObj, "name");
	}

	channel.icon = Utils::Json::getString(cObj, "icon");
}

void DiscordClient::parseOverwrites(const rapidjson::Value &ows, std::vector<Overwrite> &overwrites) {
	overwrites.clear();
	for (rapidjson::SizeType o = 0; o < ows.Size(); o++) {
		const rapidjson::Value &ow = ows[o];
		Overwrite overwrite;
		overwrite.id = Utils::Json::getString(ow, "id");
		overwrite.type = Utils::Json::getInt(ow, "type");
		overwrite.allow = Utils::Json::getUint64(ow, "allow");
		overwrite.deny = Utils::Json::getUint64(ow, "deny");
		overwrites.push_back(std::move(overwrite));
	}
}

static std::string computeMuteEndTime(bool muted, int timeWindowMinutes) {
	if (!muted || timeWindowMinutes <= 0) {
		return "";
	}
	time_t endUnix = time(nullptr) + (time_t)timeWindowMinutes * 60;
	struct tm gmt;
	if (!gmtime_r(&endUnix, &gmt)) {
		return "";
	}
	char buf[64];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000000+00:00", gmt.tm_year + 1900, gmt.tm_mon + 1,
	         gmt.tm_mday, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
	return buf;
}

// mute_config.selected_time_window is in seconds, or -1 for an indefinite mute.
static std::string muteConfigJson(bool muted, int timeWindowMinutes, const std::string &endTime) {
	if (!muted) {
		return "{\"muted\":false,\"mute_config\":null}";
	}
	if (timeWindowMinutes <= 0 || endTime.empty()) {
		return "{\"muted\":true,\"mute_config\":{\"selected_time_window\":-1,\"end_time\":null}}";
	}
	return "{\"muted\":true,\"mute_config\":{\"selected_time_window\":" + std::to_string(timeWindowMinutes * 60) +
	       ",\"end_time\":\"" + endTime + "\"}}";
}

void DiscordClient::setGuildMuted(const std::string &guildId, bool muted, int timeWindowMinutes) {
	if (token.empty() || guildId.empty()) {
		return;
	}

	std::string endTime = computeMuteEndTime(muted, timeWindowMinutes);

	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		auto &gs = notificationSettings[guildId];
		gs.guildId = guildId;
		gs.muted = muted;
		gs.muteEndTime = endTime;
	}

	std::string url = "https://discord.com/api/v10/users/@me/guilds/" + guildId + "/settings";
	std::string body = muteConfigJson(muted, timeWindowMinutes, endTime);

	Network::NetworkManager::getInstance().enqueue(
	    url, "PATCH", body, Network::RequestPriority::INTERACTIVE,
	    [guildId, muted](const Network::HttpResponse &resp) {
		    if (resp.success) {
			    Logger::log("[API] Guild %s mute set to %s", guildId.c_str(), muted ? "true" : "false");
		    } else {
			    Logger::log("[API] Failed to set guild mute: %d %s", resp.statusCode, resp.error.c_str());
		    }
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

void DiscordClient::setChannelMuted(const std::string &channelId, bool muted, int timeWindowMinutes) {
	if (token.empty() || channelId.empty()) {
		return;
	}
	std::string guildId = getGuildIdFromChannel(channelId);
	if (guildId.empty()) {
		return;
	}

	std::string endTime = computeMuteEndTime(muted, timeWindowMinutes);

	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		auto &co = notificationSettings[guildId].channelOverrides[channelId];
		co.channelId = channelId;
		co.muted = muted;
		co.muteEndTime = endTime;
	}

	// Modify takes channel_overrides as a map, unlike the array returned when reading.
	std::string body =
	    "{\"channel_overrides\":{\"" + channelId + "\":" + muteConfigJson(muted, timeWindowMinutes, endTime) + "}}";

	std::string url = "https://discord.com/api/v10/users/@me/guilds/" + guildId + "/settings";
	Network::NetworkManager::getInstance().enqueue(
	    url, "PATCH", body, Network::RequestPriority::INTERACTIVE,
	    [channelId, muted](const Network::HttpResponse &resp) {
		    if (resp.success) {
			    Logger::log("[API] Channel %s mute set to %s", channelId.c_str(), muted ? "true" : "false");
		    } else {
			    Logger::log("[API] Failed to set channel mute: %d %s", resp.statusCode, resp.error.c_str());
		    }
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

void DiscordClient::setGuildNotificationLevel(const std::string &guildId, int messageNotifications) {
	if (token.empty() || guildId.empty() || messageNotifications < 0 || messageNotifications > 2) {
		return;
	}

	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		auto &gs = notificationSettings[guildId];
		gs.guildId = guildId;
		gs.messageNotifications = messageNotifications;
		// The flag bits would otherwise keep overriding message_notifications.
		gs.flags &= ~((1 << 11) | (1 << 12));
	}

	std::string url = "https://discord.com/api/v10/users/@me/guilds/" + guildId + "/settings";
	std::string body = "{\"message_notifications\":" + std::to_string(messageNotifications) + "}";

	Network::NetworkManager::getInstance().enqueue(
	    url, "PATCH", body, Network::RequestPriority::INTERACTIVE,
	    [guildId, messageNotifications](const Network::HttpResponse &resp) {
		    if (resp.success) {
			    Logger::log("[API] Guild %s notification level set to %d", guildId.c_str(), messageNotifications);
		    } else {
			    Logger::log("[API] Failed to set guild notification level: %d %s", resp.statusCode, resp.error.c_str());
		    }
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

void DiscordClient::setChannelNotificationLevel(const std::string &channelId, int messageNotifications) {
	if (token.empty() || channelId.empty() || messageNotifications < 0 || messageNotifications > 3) {
		return;
	}
	std::string guildId = getGuildIdFromChannel(channelId);
	if (guildId.empty() || guildId == "DM") {
		return;
	}

	{
		std::lock_guard<std::recursive_mutex> lock(clientMutex);
		auto &co = notificationSettings[guildId].channelOverrides[channelId];
		co.channelId = channelId;
		co.messageNotifications = messageNotifications;
		co.flags &= ~((1 << 9) | (1 << 10));
	}

	std::string url = "https://discord.com/api/v10/users/@me/guilds/" + guildId + "/settings";
	std::string body = "{\"channel_overrides\":{\"" + channelId +
	                   "\":{\"message_notifications\":" + std::to_string(messageNotifications) + "}}}";

	Network::NetworkManager::getInstance().enqueue(
	    url, "PATCH", body, Network::RequestPriority::INTERACTIVE,
	    [channelId, messageNotifications](const Network::HttpResponse &resp) {
		    if (resp.success) {
			    Logger::log("[API] Channel %s notification level set to %d", channelId.c_str(), messageNotifications);
		    } else {
			    Logger::log("[API] Failed to set channel notification level: %d %s", resp.statusCode,
			                resp.error.c_str());
		    }
	    },
	    {{"Authorization", token}, {"Content-Type", "application/json"}});
}

} // namespace Discord
