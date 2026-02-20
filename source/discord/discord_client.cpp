#include "discord/discord_client.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "log.h"
#include "network/http_client.h"
#include "network/network_manager.h"
#include "utils/json_utils.h"
#include "utils/message_utils.h"
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace Discord {

namespace {
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
  if (s == "online")
    return UserStatus::ONLINE;
  if (s == "idle")
    return UserStatus::IDLE;
  if (s == "dnd")
    return UserStatus::DND;
  if (s == "invisible")
    return UserStatus::INVISIBLE;
  if (s == "offline")
    return UserStatus::OFFLINE;
  return UserStatus::UNKNOWN;
}
} // namespace

DiscordClient &DiscordClient::getInstance() {
  static DiscordClient instance;
  return instance;
}

void DiscordClient::init() {}

DiscordClient::DiscordClient()
    : state(ConnectionState::DISCONNECTED), heartbeatInterval(0),
      lastHeartbeat(0), waitingForHeartbeatAck(false), hasReceivedHello(false),
      sessionId(""), lastSequence(0), isConnecting(false), stopWorker(false) {

  workerThread = std::thread(&DiscordClient::workerLoop, this);
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
  if (state != ConnectionState::DISCONNECTED &&
      state != ConnectionState::DISCONNECTED_ERROR) {
    Logger::log("Connect called but state is %d", (int)state);
    return false;
  }

  if (isConnecting) {
    Logger::log("Connect called but already in progress");
    return false;
  }

  this->token = token;
  isConnecting = true;
  setState(ConnectionState::CONNECTING, "Starting network thread...");

  if (networkThread.joinable()) {
    networkThread.join();
  }

  {
    std::lock_guard<std::mutex> lock(sendQueueMutex);
    sendQueue.clear();
  }

  networkThread = std::thread(&DiscordClient::runNetworkThread, this, token);
  return true;
}

void DiscordClient::logout() {
  disconnect();
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  sessionId.clear();
  lastSequence = 0;
  guilds.clear();
  folders.clear();
  currentUser = User();
  self = User();
  token.clear();
  selectedGuildId.clear();
  selectedChannelId.clear();
  setState(ConnectionState::DISCONNECTED, "Logged out");
}

void DiscordClient::disconnect() {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  if (state == ConnectionState::DISCONNECTED)
    return;

  Logger::log("DiscordClient::disconnect called");

  setState(ConnectionState::DISCONNECTED, "Disconnected");

  ws.disconnect();

  if (networkThread.joinable()) {
    networkThread.join();
  }

  isConnecting = false;

  {
    std::lock_guard<std::mutex> qLock(queueMutex);
    while (!messageQueue.empty())
      messageQueue.pop_front();
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
    ws.setOnMessage([this](std::string &msg) { handleMessage(msg); });

    ws.setOnError([this](const std::string &err) {
      setStatus("Error: " + err);
      Logger::log("[Gateway] Error: %s", err.c_str());
    });

    ws.setOnClose([this](int code, const std::string &reason) {
      Logger::log("[Gateway] Closed: %d %s", code, reason.c_str());
      setStatus("Disconnected: " + std::to_string(code));
    });

    std::string gatewayUrl = DISCORD_GATEWAY_URL;

    setStatus(Core::I18n::getInstance().get("login.status.connecting"));
    if (!ws.connect(gatewayUrl)) {
      setStatus(Core::I18n::getInstance().get("login.status.connect_failed"));

      uint64_t delay = sessionId.empty() ? 5 : 0;

      if (delay > 0) {
        svcSleepThread(delay * 1000 * 1000 * 1000);
      }
      continue;
    }

    setStatus(Core::I18n::getInstance().get("login.status.waiting_hello"));
    {
      std::lock_guard<std::recursive_mutex> lock(clientMutex);
      isConnecting = false;
    }

    while (ws.isConnected() && state != ConnectionState::DISCONNECTED) {
      ws.poll();

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
        if (now - lastHeartbeat >= (uint64_t)heartbeatInterval) {
          if (waitingForHeartbeatAck) {
            Logger::log("[Gateway] Heartbeat ACK missing, reconnecting...");

            ws.disconnect();
            break;
          }
          sendHeartbeat();
          lastHeartbeat = now;
          waitingForHeartbeatAck = true;
        }
      }

      svcSleepThread(5ULL * 1000 * 1000);
    }

    if (state == ConnectionState::DISCONNECTED) {
      break;
    }

    uint64_t retryDelay = sessionId.empty() ? 1 : 0;

    if (sessionId.empty()) {
      Logger::log("[Gateway] Login or critical error, retrying...");
    } else {
      Logger::log(
          "[Gateway] Connection lost, attempting immediate reconnection...");
    }

    setStatus(Core::I18n::getInstance().get("login.status.lost_connection"));
    ws.disconnect();
    if (retryDelay > 0) {
      svcSleepThread(retryDelay * 1000 * 1000 * 1000);
    }
  }

  Logger::log("[Network] Thread stopped");
}

void DiscordClient::workerLoop() {
  Logger::log("[Worker] Message processing thread started");
  while (true) {
    std::string message;
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      queueCv.wait(lock,
                   [this] { return !messageQueue.empty() || stopWorker; });

      if (stopWorker && messageQueue.empty()) {
        break;
      }

      message = std::move(messageQueue.front());
      messageQueue.pop_front();
    }

    if (!message.empty()) {
      processMessage(message);
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
}

void DiscordClient::triggerTypingIndicator(const std::string &channelId) {
  if (!Config::getInstance().isTypingIndicatorEnabled())
    return;
  if (channelId.empty())
    return;
  std::string url =
      "https://discord.com/api/v10/channels/" + channelId + "/typing";
  Network::NetworkManager::getInstance().enqueue(
      url, "POST", "", Network::RequestPriority::INTERACTIVE,
      [](const Network::HttpResponse &) {}, {{"Authorization", token}});
}

std::vector<TypingUser>
DiscordClient::getTypingUsers(const std::string &channelId) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  if (typingUsers.find(channelId) != typingUsers.end()) {
    return typingUsers[channelId];
  }
  return {};
}

void DiscordClient::setState(ConnectionState newState,
                             const std::string &message) {
  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    state = newState;
  }
  setStatus(message);
  Logger::log("[Gateway] State: %d, Msg: %s", (int)newState, message.c_str());
}

void DiscordClient::setStatus(const std::string &message) {
  std::lock_guard<std::mutex> lock(statusMutex);
  statusMessage = message;
}

void DiscordClient::handleMessage(std::string &message) {
  if (message.empty())
    return;

  {
    std::lock_guard<std::mutex> lock(queueMutex);
    messageQueue.push_back(std::move(message));
  }
  queueCv.notify_one();
}

void DiscordClient::processMessage(std::string &message) {
  rapidjson::Document doc;

  doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(
      &message[0]);

  if (doc.HasParseError()) {
    Logger::log("JSON parse error: %s offset %u",
                rapidjson::GetParseError_En(doc.GetParseError()),
                (unsigned)doc.GetErrorOffset());
    return;
  }

  if (!doc.IsObject())
    return;

  int op = Utils::Json::getInt(doc, "op", -1);
  lastSequence = Utils::Json::getUint64(doc, "s");

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
    heartbeatInterval = Utils::Json::getUint64(d, "heartbeat_interval");
    if (heartbeatInterval > 0) {
      Logger::log("[Gateway] Hello received. Heartbeat interval: %llu ms",
                  heartbeatInterval);

      lastHeartbeat = osGetTime();
      sendHeartbeat();
      setStatus(Core::I18n::getInstance().get("login.status.authenticating"));

      if (!sessionId.empty() && lastSequence > 0) {
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

  if (t == "READY") {
    handleReady(d);
  } else if (t == "GUILD_CREATE") {
    handleGuildCreate(d);
  } else if (t == "CHANNEL_CREATE" || t == "CHANNEL_UPDATE") {
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
  } else if (t == "PRESENCE_UPDATE") {
    handlePresenceUpdate(d);
  } else if (t == "USER_SETTINGS_UPDATE") {
    handleUserSettingsUpdate(d);
  } else if (t == "SESSIONS_REPLACE") {
    handleSessionsReplace(d);
  } else if (t == "THREAD_CREATE" || t == "THREAD_UPDATE") {
    handleChannelCreateUpdate(d);
  } else if (t == "THREAD_LIST_SYNC") {
    if (d.HasMember("threads") && d["threads"].IsArray()) {
      const rapidjson::Value &threads = d["threads"];
      for (rapidjson::SizeType i = 0; i < threads.Size(); i++) {
        handleChannelCreateUpdate(threads[i]);
      }
    }
  }
}

void DiscordClient::handleReady(const rapidjson::Value &d) {
  std::string newSessionId;
  User newCurrentUser;
  std::vector<Guild> newGuilds;
  std::vector<GuildFolder> newGuildFolders;

  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    channelToGuildCache.clear();
  }

  newSessionId = Utils::Json::getString(d, "session_id");
  if (!newSessionId.empty()) {
    Logger::log("[Gateway] READY: Session ID = %s", newSessionId.c_str());
  }

  if (d.HasMember("user") && d["user"].IsObject()) {
    const rapidjson::Value &user = d["user"];
    newCurrentUser.id = Utils::Json::getString(user, "id");
    newCurrentUser.username = Utils::Json::getString(user, "username");
    newCurrentUser.global_name = Utils::Json::getString(user, "global_name");
    newCurrentUser.avatar = Utils::Json::getString(user, "avatar");
    newCurrentUser.discriminator =
        Utils::Json::getString(user, "discriminator");
  }

  if (d.HasMember("sessions") && d["sessions"].IsArray()) {
    const rapidjson::Value &sessions = d["sessions"];
    for (rapidjson::SizeType i = 0; i < sessions.Size(); i++) {
      const rapidjson::Value &s = sessions[i];
      if (Utils::Json::getString(s, "session_id") == newSessionId) {
        newCurrentUser.status =
            stringToStatus(Utils::Json::getString(s, "status"));
        break;
      }
    }
  }

  if (connectionCallback) {
    connectionCallback();
  }

  if (d.HasMember("guilds") && d["guilds"].IsArray()) {
    const rapidjson::Value &guildsArr = d["guilds"];
    Logger::log("[Gateway] Parsing %u guilds...", guildsArr.Size());
    setStatus(Core::I18n::getInstance().get("login.status.loading_guilds") +
              " (0/" + std::to_string(guildsArr.Size()) + ")...");

    for (rapidjson::SizeType i = 0; i < guildsArr.Size(); i++) {
      if (i % 5 == 0) {
        setStatus(Core::I18n::getInstance().get("login.status.loading_guilds") +
                  " (" + std::to_string(i) + "/" +
                  std::to_string(guildsArr.Size()) + ")...");
      }

      const rapidjson::Value &gObj = guildsArr[i];
      Guild guild;
      parseGuildObject(gObj, guild, newCurrentUser.id);
      newGuilds.push_back(std::move(guild));
    }
  }

  std::vector<Channel> newPrivateChannels;
  if (d.HasMember("private_channels") && d["private_channels"].IsArray()) {
    const rapidjson::Value &pcs = d["private_channels"];
    Logger::log("[Gateway] Parsing %u private channels...", pcs.Size());
    for (rapidjson::SizeType i = 0; i < pcs.Size(); i++) {
      Channel channel;
      parseChannelObject(pcs[i], channel);
      newPrivateChannels.push_back(std::move(channel));
    }
  }

  setStatus("Processing user settings...");
  if (d.HasMember("user_settings") && d["user_settings"].IsObject()) {
    const rapidjson::Value &settings = d["user_settings"];
    if (settings.HasMember("guild_folders") &&
        settings["guild_folders"].IsArray()) {
      std::vector<std::string> sortOrder;
      const rapidjson::Value &foldersArr = settings["guild_folders"];

      for (rapidjson::SizeType i = 0; i < foldersArr.Size(); i++) {
        const rapidjson::Value &folderObj = foldersArr[i];

        GuildFolder folder;
        if (folderObj.HasMember("id") && folderObj["id"].IsString()) {
          folder.id = folderObj["id"].GetString();
        } else if (folderObj.HasMember("id") && folderObj["id"].IsInt64()) {
          folder.id = std::to_string(folderObj["id"].GetInt64());
        }
        folder.name = Utils::Json::getString(folderObj, "name");
        folder.color = Utils::Json::getInt(folderObj, "color");

        if (folderObj.HasMember("guild_ids") &&
            folderObj["guild_ids"].IsArray()) {
          const rapidjson::Value &ids = folderObj["guild_ids"];
          for (rapidjson::SizeType j = 0; j < ids.Size(); j++) {
            if (ids[j].IsString()) {
              std::string gid = ids[j].GetString();
              folder.guildIds.push_back(gid);
              sortOrder.push_back(gid);
            }
          }
        }
        newGuildFolders.push_back(folder);
      }

      if (!sortOrder.empty()) {
        setStatus("Sorting guilds...");
        std::vector<Guild> sortedGuilds;
        std::vector<Guild> remainingGuilds = std::move(newGuilds);

        for (const auto &id : sortOrder) {
          for (auto it = remainingGuilds.begin();
               it != remainingGuilds.end();) {
            if (it->id == id) {
              sortedGuilds.push_back(std::move(*it));
              it = remainingGuilds.erase(it);
              break;
            } else {
              ++it;
            }
          }
        }

        for (auto &g : remainingGuilds) {
          sortedGuilds.push_back(std::move(g));
        }

        newGuilds = std::move(sortedGuilds);
        Logger::log("Guilds sorted (local pre-lock).");
      }
    }
  }

  setStatus("Finalizing login...");
  Logger::log("[Gateway] Locking clientMutex to finalize READY...");
  {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    sessionId = newSessionId;
    currentUser = newCurrentUser;
    guilds = std::move(newGuilds);
    privateChannels = std::move(newPrivateChannels);
    folders = std::move(newGuildFolders);

    std::string accName = currentUser.username;
    Config::getInstance().updateCurrentAccountName(accName);

    setState(ConnectionState::READY,
             "Ready! Logged in as " + currentUser.username);
  }
}

void DiscordClient::handleGuildCreate(const rapidjson::Value &d) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);

  Guild guild;
  parseGuildObject(d, guild, currentUser.id);

  bool found = false;
  for (auto &g : guilds) {
    if (g.id == guild.id) {
      g.name = guild.name;
      g.icon = guild.icon;
      g.ownerId = guild.ownerId;
      if (!guild.roles.empty())
        g.roles = std::move(guild.roles);
      if (!guild.members.empty())
        g.members = std::move(guild.members);
      if (!guild.myRoles.empty())
        g.myRoles = std::move(guild.myRoles);
      g.channels = std::move(guild.channels);
      Logger::log("Updated existing guild %s (merged)", g.name.c_str());
      found = true;
      break;
    }
  }
  if (!found) {
    guilds.push_back(std::move(guild));
    Logger::log("Added new guild %s", guilds.back().name.c_str());
  }
}

void DiscordClient::handleChannelCreateUpdate(const rapidjson::Value &d) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);

  Channel channel;
  parseChannelObject(d, channel);

  if (channel.type == 1 || channel.type == 3) { // DM or Group DM
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
    Logger::log("Updated DM channel %s (%s)", channel.name.c_str(),
                channel.id.c_str());
  } else if (d.HasMember("guild_id")) {
    std::string guildId = Utils::Json::getString(d, "guild_id");

    for (auto &guild : guilds) {
      if (guild.id == guildId) {
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

        // Recalculate viewable flag
        uint64_t finalPerms = computeChannelPermissions(
            guild, channel, currentUser.id, guild.myRoles);

        // Update the channel in the list again with the flag
        for (auto &c : guild.channels) {
          if (c.id == channel.id) {
            c.viewable = (finalPerms & Permissions::VIEW_CHANNEL) != 0;
            break;
          }
        }

        Logger::log("Updated guild channel %s (%s) in guild %s",
                    channel.name.c_str(), channel.id.c_str(),
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

  Logger::log("TYPING_START: channel=%s user=%s (me=%s)", channelId.c_str(),
              userId.c_str(), currentUser.id.c_str());

  if (userId == currentUser.id)
    return;

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
    Logger::log("Added typing user %s to channel %s", userId.c_str(),
                channelId.c_str());
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
}

void DiscordClient::handleMessageUpdate(const rapidjson::Value &d) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  Message msg = parseSingleMessage(d);

  if (messageUpdateCallback) {
    messageUpdateCallback(msg);
  }
}

void DiscordClient::handleMessageDelete(const rapidjson::Value &d) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  std::string id = Utils::Json::getString(d, "id");

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
    const rapidjson::Value &e = d["emoji"];
    emoji.id = Utils::Json::getString(e, "id");
    emoji.name = Utils::Json::getString(e, "name");
    emoji.animated = e.HasMember("animated") && e["animated"].IsBool() &&
                     e["animated"].GetBool();
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
    const rapidjson::Value &e = d["emoji"];
    emoji.id = Utils::Json::getString(e, "id");
    emoji.name = Utils::Json::getString(e, "name");
    emoji.animated = e.HasMember("animated") && e["animated"].IsBool() &&
                     e["animated"].GetBool();
  }

  if (messageReactionRemoveCallback) {
    messageReactionRemoveCallback(channelId, messageId, userId, emoji);
  }
}

void DiscordClient::handlePresenceUpdate(const rapidjson::Value &d) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  if (!d.HasMember("user") || !d["user"].IsObject())
    return;
  std::string userId = Utils::Json::getString(d["user"], "id");

  if (userId == currentUser.id) {
    std::string statusStr = Utils::Json::getString(d, "status");
    currentUser.status = stringToStatus(statusStr);
    Logger::log("[Gateway] Own presence updated via PRESENCE_UPDATE to %s",
                statusStr.c_str());
  }
}

void DiscordClient::handleUserSettingsUpdate(const rapidjson::Value &d) {
  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  if (d.HasMember("status") && d["status"].IsString()) {
    std::string statusStr = d["status"].GetString();
    currentUser.status = stringToStatus(statusStr);
    Logger::log("[Gateway] Own status updated via USER_SETTINGS_UPDATE to %s",
                statusStr.c_str());
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
        Logger::log("[Gateway] Own status updated via SESSIONS_REPLACE to %s",
                    statusStr.c_str());
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

Message DiscordClient::parseSingleMessage(const rapidjson::Value &d) {
  Message msg;
  msg.id = Utils::Json::getString(d, "id");
  msg.content = Utils::Json::getString(d, "content");
  msg.timestamp = Utils::Json::getString(d, "timestamp");
  msg.edited_timestamp = Utils::Json::getString(d, "edited_timestamp");
  msg.channelId = Utils::Json::getString(d, "channel_id");

  if (d.HasMember("author") && d["author"].IsObject()) {
    const rapidjson::Value &author = d["author"];
    msg.author.id = Utils::Json::getString(author, "id");
    msg.author.username = Utils::Json::getString(author, "username");
    msg.author.global_name = Utils::Json::getString(author, "global_name");
    msg.author.avatar = Utils::Json::getString(author, "avatar");
    msg.author.discriminator = Utils::Json::getString(author, "discriminator");
  }

  // Parse partial member data from message (for role colors)
  if (d.HasMember("member") && d["member"].IsObject()) {
    const rapidjson::Value &memObj = d["member"];
    msg.member.user_id = msg.author.id;
    msg.member.nickname = Utils::Json::getString(memObj, "nick");

    if (memObj.HasMember("roles") && memObj["roles"].IsArray()) {
      const rapidjson::Value &rIds = memObj["roles"];
      for (rapidjson::SizeType r = 0; r < rIds.Size(); r++) {
        if (rIds[r].IsString()) {
          msg.member.role_ids.push_back(rIds[r].GetString());
        }
      }
    }
  }

  if (d.HasMember("embeds") && d["embeds"].IsArray()) {
    const rapidjson::Value &embeds = d["embeds"];
    for (rapidjson::SizeType e = 0; e < embeds.Size(); e++) {
      const rapidjson::Value &eObj = embeds[e];
      Embed embed;
      embed.title = Utils::Json::getString(eObj, "title");
      embed.description = Utils::Json::getString(eObj, "description");
      embed.url = Utils::Json::getString(eObj, "url");
      embed.type = Utils::Json::getString(eObj, "type");
      embed.color = Utils::Json::getInt(eObj, "color");
      embed.timestamp = Utils::Json::getString(eObj, "timestamp");

      if (eObj.HasMember("author") && eObj["author"].IsObject()) {
        embed.author_name = Utils::Json::getString(eObj["author"], "name");
        embed.author_icon_url =
            Utils::Json::getString(eObj["author"], "icon_url");
      }
      if (eObj.HasMember("footer") && eObj["footer"].IsObject()) {
        embed.footer_text = Utils::Json::getString(eObj["footer"], "text");
        embed.footer_icon_url =
            Utils::Json::getString(eObj["footer"], "icon_url");
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
      if (eObj.HasMember("fields") && eObj["fields"].IsArray()) {
        const rapidjson::Value &fields = eObj["fields"];
        for (rapidjson::SizeType f = 0; f < fields.Size() && f < 10; f++) {
          const rapidjson::Value &fObj = fields[f];
          EmbedField field;
          field.name = Utils::Json::getString(fObj, "name");
          field.value = Utils::Json::getString(fObj, "value");
          field.isInline = fObj.HasMember("inline") && fObj["inline"].IsBool()
                               ? fObj["inline"].GetBool()
                               : false;
          embed.fields.push_back(field);
        }
      }
      msg.embeds.push_back(embed);
    }
  }

  if (d.HasMember("attachments") && d["attachments"].IsArray()) {
    const rapidjson::Value &attachments = d["attachments"];
    for (rapidjson::SizeType a = 0; a < attachments.Size(); a++) {
      const rapidjson::Value &aObj = attachments[a];
      Attachment attachment;
      attachment.id = Utils::Json::getString(aObj, "id");
      attachment.filename = Utils::Json::getString(aObj, "filename");
      attachment.url = Utils::Json::getString(aObj, "url");
      attachment.proxy_url = Utils::Json::getString(aObj, "proxy_url");
      attachment.size = Utils::Json::getInt(aObj, "size");
      attachment.width = Utils::Json::getInt(aObj, "width");
      attachment.height = Utils::Json::getInt(aObj, "height");
      attachment.content_type = Utils::Json::getString(aObj, "content_type");
      msg.attachments.push_back(attachment);
    }
  }

  // Parse stickers
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
      reaction.me = rObj.HasMember("me") && rObj["me"].IsBool()
                        ? rObj["me"].GetBool()
                        : false;

      if (rObj.HasMember("emoji") && rObj["emoji"].IsObject()) {
        const rapidjson::Value &eObj = rObj["emoji"];
        reaction.emoji.id = Utils::Json::getString(eObj, "id");
        reaction.emoji.name = Utils::Json::getString(eObj, "name");
      }
      msg.reactions.push_back(reaction);
    }
  }

  // Parse message type and reply reference
  msg.type = Utils::Json::getInt(d, "type");

  if (d.HasMember("message_snapshots") && d["message_snapshots"].IsArray()) {
    const rapidjson::Value &snapshots = d["message_snapshots"];
    if (snapshots.Size() > 0 && snapshots[0].HasMember("message") &&
        snapshots[0]["message"].IsObject()) {
      msg.isForwarded = true;
      const rapidjson::Value &innerMsg = snapshots[0]["message"];
      if (msg.content.empty()) {
        msg.content = Utils::Json::getString(innerMsg, "content");
      }
      if (innerMsg.HasMember("author") && innerMsg["author"].IsObject()) {
        const rapidjson::Value &innerAuthor = innerMsg["author"];
        std::string gname = Utils::Json::getString(innerAuthor, "global_name");
        msg.originalAuthorName =
            gname.empty() ? Utils::Json::getString(innerAuthor, "username")
                          : gname;
        msg.originalAuthorAvatar =
            Utils::Json::getString(innerAuthor, "avatar");
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
      msg.referencedAuthorName =
          gname.empty() ? Utils::Json::getString(refAuthor, "username") : gname;

      bool hasRefMember =
          (refMsg.HasMember("member") && refMsg["member"].IsObject());
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
            if (roles[i].IsString())
              temp.role_ids.push_back(roles[i].GetString());
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
            msg.referencedAuthorColor =
                m.role_ids.empty() ? 0 : getRoleColor(guildId, m);
          }
        }
      }
    }
  }

  return msg;
}

void DiscordClient::sendHeartbeat() {
  char buffer[128];
  if (lastSequence != 0) {
    snprintf(buffer, sizeof(buffer), "{\"op\": 1, \"d\": %llu}",
             (unsigned long long)lastSequence);
  } else {
    snprintf(buffer, sizeof(buffer), "{\"op\": 1, \"d\": null}");
  }
  queueSend(buffer);
  Logger::log("[Gateway] Sent Heartbeat");
}

void DiscordClient::sendIdentify() {

  std::string json = "{"
                     "\"op\": 2,"
                     "\"d\": {"
                     "\"token\": \"" +
                     token +
                     "\","
                     "\"properties\": {"
                     "\"os\": \"Nintendo 3DS\","
                     "\"browser\": \"TriCord\","
                     "\"device\": \"Nintendo 3DS\""
                     "},"
                     "\"compress\": false,"
                     "\"large_threshold\": 50"
                     "}"
                     "}";
  queueSend(json);
  Logger::log("[Gateway] Sent Identify");
}

void DiscordClient::sendResume() {
  std::string json = "{"
                     "\"op\": 6,"
                     "\"d\": {"
                     "\"token\": \"" +
                     token +
                     "\","
                     "\"session_id\": \"" +
                     sessionId +
                     "\","
                     "\"seq\": " +
                     std::to_string(lastSequence) +
                     "}"
                     "}";
  queueSend(json);
  Logger::log("[Gateway] Sent Resume (seq: %llu)", lastSequence);
}

void DiscordClient::handleInvalidSession(const rapidjson::Document &doc) {
  bool resumable = Utils::Json::getBool(doc, "d");
  Logger::log("[Gateway] Invalid Session. Resumable: %d", resumable);
  if (resumable) {
    sendResume();
  } else {
    sessionId = "";
    lastSequence = 0;
    sendIdentify();
  }
}

void DiscordClient::handleReconnect() {
  Logger::log("[Gateway] Reconnect requested (Op 7)");
  ws.disconnect();
}

void DiscordClient::fetchMessagesAsync(const std::string &channelId, int limit,
                                       MessagesCallback cb,
                                       const std::string &aroundId) {
  if (channelId.empty() || token.empty()) {
    if (cb)
      cb({});
    return;
  }

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages?limit=" + std::to_string(limit);
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

            Logger::log("Fetched 0 messages for channel %s. Body len: %zu",
                        channelId.c_str(), resp.body.size());
          }
        } else {
          Logger::log("Failed to fetch messages for %s: Status %d",
                      channelId.c_str(), resp.statusCode);
          Logger::log("Response body: %s", resp.body.c_str());
        }
        if (cb)
          cb(messages);
      },
      {{"Authorization", token}});
}

void DiscordClient::fetchMessagesBeforeAsync(const std::string &channelId,
                                             const std::string &beforeId,
                                             int limit, MessagesCallback cb) {
  if (channelId.empty() || token.empty() || beforeId.empty()) {
    if (cb)
      cb({});
    return;
  }

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages?limit=" + std::to_string(limit) +
                    "&before=" + beforeId;

  Network::NetworkManager::getInstance().enqueue(
      url, "GET", "", Network::RequestPriority::BACKGROUND,
      [this, cb, channelId](const Network::HttpResponse &resp) {
        std::vector<Message> messages;
        if (resp.success && resp.statusCode == 200) {
          messages = parseMessages(resp.body);
        } else {
          Logger::log("Failed to fetch older messages for %s: Status %d",
                      channelId.c_str(), resp.statusCode);
        }
        if (cb)
          cb(messages);
      },
      {{"Authorization", token}});
}

void DiscordClient::fetchMessage(const std::string &channelId,
                                 const std::string &messageId,
                                 SingleMessageCallback cb) {
  if (channelId.empty() || messageId.empty() || token.empty()) {
    if (cb)
      cb(std::nullopt);
    return;
  }

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages/" + messageId;

  Network::NetworkManager::getInstance().enqueue(
      url, "GET", "", Network::RequestPriority::INTERACTIVE,
      [this, cb](const Network::HttpResponse &resp) {
        if (resp.success && resp.statusCode == 200) {
          rapidjson::Document doc;
          doc.Parse(resp.body.c_str());
          if (!doc.HasParseError() && doc.IsObject()) {
            if (cb)
              cb(parseSingleMessage(doc));
            return;
          }
        }
        if (cb)
          cb(std::nullopt);
      },
      {{"Authorization", token}});
}

std::vector<Message> DiscordClient::parseMessages(const std::string &json) {
  std::vector<Message> messages;
  rapidjson::Document doc;

  if (json.empty())
    return messages;
  std::string buffer = json;
  doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(
      &buffer[0]);

  if (!doc.HasParseError() && doc.IsArray()) {
    for (rapidjson::SizeType i = 0; i < doc.Size(); i++) {
      messages.push_back(parseSingleMessage(doc[i]));
    }
  }

  return messages;
}

Channel DiscordClient::getChannel(const std::string &channelId) {
  for (const auto &guild : guilds) {
    for (const auto &channel : guild.channels) {
      if (channel.id == channelId) {
        return channel;
      }
    }
  }
  for (const auto &channel : privateChannels) {
    if (channel.id == channelId) {
      return channel;
    }
  }
  return Channel();
}

Member DiscordClient::getMember(const std::string &guildId,
                                const std::string &userId) {
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

int DiscordClient::getRoleColor(const std::string &guildId,
                                const Member &member) {
  if (member.role_ids.empty())
    return 0;

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

int DiscordClient::getRoleColor(const std::string &guildId,
                                const std::string &userId) {
  Member member = getMember(guildId, userId);
  if (!member.user_id.empty()) {
    return getRoleColor(guildId, member);
  }
  return 0;
}

std::string DiscordClient::getMemberDisplayName(const std::string &guildId,
                                                const std::string &userId,
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
  if (channelId.empty())
    return "";

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

void DiscordClient::fetchGuildDetails(const std::string &guildId,
                                      std::function<void(bool)> cb) {
  if (token.empty() || guildId.empty()) {
    if (cb)
      cb(false);
    return;
  }

  std::string url =
      "https://discord.com/api/v10/guilds/" + guildId + "?with_counts=true";

  Network::NetworkManager::getInstance().enqueue(
      url, "GET", "", Network::RequestPriority::INTERACTIVE,
      [this, guildId, cb](const Network::HttpResponse &resp) {
        if (resp.success) {
          rapidjson::Document doc;
          doc.Parse(resp.body.c_str());

          if (!doc.HasParseError() && doc.IsObject()) {
            std::lock_guard<std::recursive_mutex> lock(clientMutex);
            for (auto &g : guilds) {
              if (g.id == guildId) {
                parseGuildObject(doc, g, currentUser.id);
                break;
              }
            }
            if (cb)
              cb(true);
            return;
          }
        }
        if (cb)
          cb(false);
      },
      {{"Authorization", token}});
}

Message DiscordClient::parseSingleMessage(const std::string &json) {
  rapidjson::Document doc;
  std::string buffer = json;
  doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(
      &buffer[0]);

  if (doc.HasParseError() || !doc.IsObject())
    return Message();

  return parseSingleMessage(doc);
}

uint64_t DiscordClient::calcBasePermissions(
    const Guild &guild, const std::string &userId,
    const std::vector<std::string> &memberRoleIds) {

  if (!userId.empty() && userId == guild.ownerId) {
    return ~0ULL;
  }

  uint64_t permissions = 0;

  // 1. @everyone role (ID == Guild ID)
  for (const auto &role : guild.roles) {
    if (role.id == guild.id) {
      permissions |= role.permissions;
      break;
    }
  }

  // 2. Member roles
  for (const auto &roleId : memberRoleIds) {
    for (const auto &role : guild.roles) {
      if (role.id == roleId) {
        permissions |= role.permissions;
        break;
      }
    }
  }

  // 3. Administrator check
  if (permissions & Permissions::ADMINISTRATOR) {
    return ~0ULL; // Grant all permissions
  }

  return permissions;
}

uint64_t DiscordClient::computeChannelPermissions(
    const Guild &guild, const Channel &channel, const std::string &userId,
    const std::vector<std::string> &memberRoleIds) {
  uint64_t basePerms = calcBasePermissions(guild, userId, memberRoleIds);

  // Administrator overrides everything
  if (basePerms & Permissions::ADMINISTRATOR) {
    return ~0ULL;
  }

  uint64_t perms = basePerms;

  // 1. Apply Category Overwrites if exists
  if (!channel.parent_id.empty()) {
    for (const auto &cat : guild.channels) {
      if (cat.id == channel.parent_id) {
        perms = computeOverwrites(perms, guild.id, userId, memberRoleIds,
                                  cat.permission_overwrites);
        break;
      }
    }
  }

  // 2. Apply Channel Overwrites
  perms = computeOverwrites(perms, guild.id, userId, memberRoleIds,
                            channel.permission_overwrites);

  return perms;
}

uint64_t DiscordClient::computeOverwrites(
    uint64_t basePermissions, const std::string &guildId,
    const std::string &memberId, const std::vector<std::string> &memberRoleIds,
    const rapidjson::Value &channelObj) {

  std::vector<Overwrite> overwrites;

  if (channelObj.HasMember("permission_overwrites") &&
      channelObj["permission_overwrites"].IsArray()) {
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

  return computeOverwrites(basePermissions, guildId, memberId, memberRoleIds,
                           overwrites);
}

uint64_t DiscordClient::computeOverwrites(
    uint64_t basePermissions, const std::string &guildId,
    const std::string &memberId, const std::vector<std::string> &memberRoleIds,
    const std::vector<Overwrite> &overwrites) {

  // Administrator overrides everything
  if (basePermissions & Permissions::ADMINISTRATOR) {
    return ~0ULL;
  }

  uint64_t permissions = basePermissions;
  uint64_t everyoneAllow = 0, everyoneDeny = 0;
  uint64_t roleAllow = 0, roleDeny = 0;
  uint64_t memberAllow = 0, memberDeny = 0;
  bool hasMemberOverwrite = false;

  for (const auto &ow : overwrites) {
    if (ow.type == 0) {       // Role
      if (ow.id == guildId) { // @everyone
        everyoneAllow = ow.allow;
        everyoneDeny = ow.deny;
      } else {
        // Check if user has this role
        for (const auto &rId : memberRoleIds) {
          if (rId == ow.id) {
            roleAllow |= ow.allow;
            roleDeny |= ow.deny;
            break;
          }
        }
      }
    } else if (ow.type == 1) { // Member
      if (ow.id == memberId) {
        memberAllow = ow.allow;
        memberDeny = ow.deny;
        hasMemberOverwrite = true;
      }
    }
  }

  // Apply @everyone
  permissions &= ~everyoneDeny;
  permissions |= everyoneAllow;

  // Apply roles
  permissions &= ~roleDeny;
  permissions |= roleAllow;

  // Apply member
  if (hasMemberOverwrite) {
    permissions &= ~memberDeny;
    permissions |= memberAllow;
  }

  return permissions;
}

void DiscordClient::sendMessage(const std::string &channelId,
                                const std::string &content,
                                SendMessageCallback cb) {
  if (token.empty() || channelId.empty() || content.empty())
    return;

  std::string url =
      "https://discord.com/api/v10/channels/" + channelId + "/messages";

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
      [this, cb](const Network::HttpResponse &resp) {
        if (!resp.success || resp.statusCode >= 400) {
          Logger::log("Failed to send message: %d (%s)", (int)resp.statusCode,
                      resp.error.c_str());
          if (cb) {
            cb(Message(), false, (int)resp.statusCode);
          }
          return;
        }

        if (cb) {
          Message msg = parseSingleMessage(resp.body);
          cb(msg, true, (int)resp.statusCode);
        }
      },
      {{"Authorization", token},
       {"Content-Type", "application/json"},
       {"X-Context-Properties", "eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="}});
}

void DiscordClient::sendReply(const std::string &channelId,
                              const std::string &content,
                              const std::string &replyToMessageId,
                              SendMessageCallback cb) {
  if (token.empty() || channelId.empty() || content.empty() ||
      replyToMessageId.empty())
    return;

  std::string url =
      "https://discord.com/api/v10/channels/" + channelId + "/messages";

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

  writer.Key("message_reference");
  writer.StartObject();
  writer.Key("message_id");
  writer.String(replyToMessageId.c_str());
  writer.EndObject();

  writer.EndObject();

  Network::NetworkManager::getInstance().enqueue(
      url, "POST", s.GetString(), Network::RequestPriority::REALTIME,
      [this, cb](const Network::HttpResponse &resp) {
        if (!resp.success || resp.statusCode >= 400) {
          Logger::log("Failed to send reply: %d (%s)", (int)resp.statusCode,
                      resp.error.c_str());
          if (cb) {
            cb(Message(), false, (int)resp.statusCode);
          }
          return;
        }

        if (cb) {
          Message msg = parseSingleMessage(resp.body);
          cb(msg, true, (int)resp.statusCode);
        }
      },
      {{"Authorization", token},
       {"Content-Type", "application/json"},
       {"X-Context-Properties", "eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="}});
}

void DiscordClient::sendMessageAsync(const std::string &channelId,
                                     const std::string &content,
                                     SuccessCallback cb) {
  if (token.empty() || channelId.empty() || content.empty()) {
    if (cb)
      cb(false);
    return;
  }

  std::string url =
      "https://discord.com/api/v10/channels/" + channelId + "/messages";

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
        if (cb)
          cb(resp.success);
      },
      {{"Authorization", token},
       {"X-Context-Properties", "eyJsb2NhdGlvbiI6ImNoYXRfaW5wdXQifQ=="}});
}

bool DiscordClient::editMessage(const std::string &channelId,
                                const std::string &messageId,
                                const std::string &content) {
  if (token.empty() || channelId.empty() || messageId.empty() ||
      content.empty())
    return false;

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages/" + messageId;

  rapidjson::StringBuffer s;
  rapidjson::Writer<rapidjson::StringBuffer> writer(s);
  writer.StartObject();
  writer.Key("content");
  writer.String(content.c_str());
  writer.EndObject();

  Network::HttpClient http;
  http.setAuthToken(token);
  http.setVerifySSL(true);

  Network::HttpResponse resp = http.patch(url, s.GetString());
  return resp.success;
}

void DiscordClient::editMessageAsync(const std::string &channelId,
                                     const std::string &messageId,
                                     const std::string &content,
                                     SuccessCallback cb) {
  if (token.empty() || channelId.empty() || messageId.empty() ||
      content.empty()) {
    if (cb)
      cb(false);
    return;
  }

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages/" + messageId;

  rapidjson::StringBuffer s;
  rapidjson::Writer<rapidjson::StringBuffer> writer(s);
  writer.StartObject();
  writer.Key("content");
  writer.String(content.c_str());
  writer.EndObject();

  Network::NetworkManager::getInstance().enqueue(
      url, "PATCH", s.GetString(), Network::RequestPriority::INTERACTIVE,
      [cb](const Network::HttpResponse &resp) {
        if (cb)
          cb(resp.success);
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

void DiscordClient::fetchForumThreads(const std::string &channelId,
                                      ThreadsCallback cb) {
  if (token.empty() || channelId.empty()) {
    if (cb)
      cb({});
    return;
  }

  auto ctx = std::make_shared<ThreadFetchContext>();

  auto performFetch = [this, channelId, cb, ctx](bool archived) {
    std::string url =
        "https://discord.com/api/v10/channels/" + channelId +
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
                    t.id = Utils::Json::getString(tObj, "id");
                    t.name = Utils::Json::getString(tObj, "name");
                    t.parent_id = Utils::Json::getString(tObj, "parent_id");
                    t.type = Utils::Json::getInt(tObj, "type", 11);
                    t.flags = Utils::Json::getInt(tObj, "flags");
                    t.message_count =
                        Utils::Json::getInt(tObj, "message_count");
                    t.last_message_id =
                        Utils::Json::getString(tObj, "last_message_id");
                    t.owner_id = Utils::Json::getString(tObj, "owner_id");

                    t.is_archived = false;
                    if (tObj.HasMember("thread_metadata") &&
                        tObj["thread_metadata"].IsObject()) {
                      const auto &meta = tObj["thread_metadata"];
                      t.is_archived = Utils::Json::getBool(meta, "archived");
                    }

                    ctx->threads.push_back(t);
                  }
                }

                if (doc.HasMember("first_messages") &&
                    doc["first_messages"].IsArray()) {
                  const rapidjson::Value &msgs = doc["first_messages"];
                  for (rapidjson::SizeType i = 0; i < msgs.Size(); i++) {
                    const rapidjson::Value &mObj = msgs[i];
                    Message msg = parseSingleMessage(mObj);

                    ThreadFetchContext::OPInfo info;
                    info.content = msg.content;
                    info.authorId = msg.author.id;

                    if (info.content.empty()) {
                      if (mObj.HasMember("attachments") &&
                          mObj["attachments"].IsArray() &&
                          mObj["attachments"].Size() > 0) {
                        info.content = "[Image]";
                      } else if (mObj.HasMember("embeds") &&
                                 mObj["embeds"].IsArray() &&
                                 mObj["embeds"].Size() > 0) {
                        info.content = "[Embed]";
                      }
                    }

                    if (!msg.author.id.empty()) {
                      if (!msg.member.nickname.empty()) {
                        info.authorName = msg.member.nickname;
                      } else {
                        info.authorName = getMemberDisplayName(
                            guildId, msg.author.id, msg.author);
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
              for (auto &g : guilds) {
                if (g.id == guildId) {
                  for (const auto &t : ctx->threads) {
                    bool exists = false;
                    for (const auto &existing : g.channels) {
                      if (existing.id == t.id) {
                        exists = true;
                        break;
                      }
                    }
                    if (!exists)
                      g.channels.push_back(t);
                  }
                  break;
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

bool DiscordClient::deleteMessage(const std::string &channelId,
                                  const std::string &messageId) {
  if (token.empty() || channelId.empty() || messageId.empty())
    return false;

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages/" + messageId;

  Network::HttpClient http;
  http.setAuthToken(token);
  http.setVerifySSL(true);

  Network::HttpResponse resp = http.del(url);
  return resp.success;
}

void DiscordClient::deleteMessageAsync(const std::string &channelId,
                                       const std::string &messageId,
                                       SuccessCallback cb) {
  if (token.empty() || channelId.empty() || messageId.empty()) {
    if (cb)
      cb(false);
    return;
  }

  std::string url = "https://discord.com/api/v10/channels/" + channelId +
                    "/messages/" + messageId;

  Network::NetworkManager::getInstance().enqueue(
      url, "DELETE", "", Network::RequestPriority::REALTIME,
      [cb](const Network::HttpResponse &resp) {
        if (cb)
          cb(resp.success);
      },
      {{"Authorization", token}});
}

void DiscordClient::exchangeTicketForToken(const std::string &ticket,
                                           TokenCallback cb) {
  Logger::log("[DiscordClient] Exchanging ticket for token");

  std::string url = "https://discord.com/api/v10/users/@me/remote-auth/login";

  rapidjson::Document doc;
  doc.SetObject();
  rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();

  doc.AddMember("ticket", rapidjson::Value(ticket.c_str(), allocator),
                allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  std::string payload = buffer.GetString();

  Logger::log("[DiscordClient] Exchange payload: %s", payload.c_str());

  Network::NetworkManager::getInstance().enqueue(
      url, "POST", payload, Network::RequestPriority::INTERACTIVE,
      [cb](const Network::HttpResponse &resp) {
        if (!resp.success) {
          Logger::log("[DiscordClient] Token exchange failed: %d",
                      resp.statusCode);
          Logger::log("[DiscordClient] Response body: %s", resp.body.c_str());
          if (cb)
            cb("");
          return;
        }

        rapidjson::Document doc;
        doc.Parse(resp.body.c_str());

        if (doc.HasParseError() || !doc.IsObject()) {
          Logger::log("[DiscordClient] Failed to parse token response");
          if (cb)
            cb("");
          return;
        }

        std::string token = Utils::Json::getString(doc, "encrypted_token");
        Logger::log("[DiscordClient] Token received: %s",
                    token.substr(0, 20).c_str());

        if (cb)
          cb(token);
      },
      {{"Content-Type", "application/json"}});
}

void DiscordClient::fetchMember(const std::string &guildId,
                                const std::string &userId, MemberCallback cb) {
  if (guildId.empty() || userId.empty()) {
    if (cb)
      cb(Member());
    return;
  }

  std::string url =
      "https://discord.com/api/v10/guilds/" + guildId + "/members/" + userId;

  Network::NetworkManager::getInstance().enqueue(
      url, "GET", "", Network::RequestPriority::BACKGROUND,
      [this, cb, userId, guildId](const Network::HttpResponse &resp) {
        if (!resp.success) {
          if (cb)
            cb(Member());
          return;
        }

        rapidjson::Document d;
        d.Parse(resp.body.c_str());

        if (d.HasParseError()) {
          if (cb)
            cb(Member());
          return;
        }

        Member member;

        if (d.HasMember("user") && d["user"].IsObject()) {
          member.user_id = Utils::Json::getString(d["user"], "id");
        } else {
          member.user_id = userId;
        }

        member.nickname = Utils::Json::getString(d, "nick");
        if (d.HasMember("roles") && d["roles"].IsArray()) {
          const rapidjson::Value &roles = d["roles"];
          for (rapidjson::SizeType i = 0; i < roles.Size(); i++) {
            if (roles[i].IsString())
              member.role_ids.push_back(roles[i].GetString());
          }
        }

        {
          std::lock_guard<std::recursive_mutex> lock(clientMutex);
          for (auto &g : guilds) {
            if (g.id == guildId) {

              bool found = false;
              for (auto &m : g.members) {
                if (m.user_id == member.user_id) {
                  m = member;
                  found = true;
                  break;
                }
              }
              if (!found) {
                g.members.push_back(member);
              }
              break;
            }
          }
        }

        if (cb) {
          cb(member);
        }
      },
      {{"Authorization", token}});
}

void DiscordClient::performLogin(const std::string &email,
                                 const std::string &password,
                                 LoginCallback cb) {
  rapidjson::Document d;
  d.SetObject();
  rapidjson::Document::AllocatorType &allocator = d.GetAllocator();

  d.AddMember("login", rapidjson::Value(email.c_str(), allocator), allocator);
  d.AddMember("password", rapidjson::Value(password.c_str(), allocator),
              allocator);
  d.AddMember("undelete", false, allocator);
  d.AddMember("captcha_key", rapidjson::Value(), allocator);
  d.AddMember("login_source", rapidjson::Value(), allocator);
  d.AddMember("gift_code_sku_id", rapidjson::Value(), allocator);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  d.Accept(writer);
  std::string json = buffer.GetString();

  Network::NetworkManager::getInstance().enqueue(
      "https://discord.com/api/v10/auth/login", "POST", json,
      Network::RequestPriority::INTERACTIVE,
      [cb](const Network::HttpResponse &resp) {
        if (resp.statusCode == 200) {
          rapidjson::Document doc;
          doc.Parse(resp.body.c_str());

          if (doc.HasParseError()) {
            if (cb)
              cb(false, "", false, "", "Response parse error");
            return;
          }

          if (doc.HasMember("mfa") && doc["mfa"].IsBool() &&
              doc["mfa"].GetBool()) {
            std::string ticket = "";
            if (doc.HasMember("ticket") && doc["ticket"].IsString()) {
              ticket = doc["ticket"].GetString();
            }
            if (cb)
              cb(false, "", true, ticket, "");
          } else if (doc.HasMember("token") && doc["token"].IsString()) {
            std::string token = doc["token"].GetString();
            if (cb)
              cb(true, token, false, "", "");
          } else {
            if (cb)
              cb(false, "", false, "", "Unknown response format");
          }
        } else {
          std::string error =
              "Login failed: " + std::to_string(resp.statusCode);
          rapidjson::Document doc;
          doc.Parse(resp.body.c_str());
          if (!doc.HasParseError()) {
            if (doc.HasMember("message") && doc["message"].IsString()) {
              error = doc["message"].GetString();
            } else if (doc.HasMember("errors")) {
              error = "Invalid credentials";
            }
          }
          if (cb)
            cb(false, "", false, "", error);
        }
      },
      {{"Content-Type", "application/json"}});
}

void DiscordClient::submitMFA(const std::string &ticket,
                              const std::string &code, LoginCallback cb) {
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
      "https://discord.com/api/v10/auth/mfa/totp", "POST", json,
      Network::RequestPriority::INTERACTIVE,
      [cb](const Network::HttpResponse &resp) {
        if (resp.statusCode == 200) {
          rapidjson::Document doc;
          doc.Parse(resp.body.c_str());
          if (doc.HasMember("token") && doc["token"].IsString()) {
            std::string token = doc["token"].GetString();
            if (cb)
              cb(true, token, false, "", "");
          } else {
            if (cb)
              cb(false, "", false, "", "No token in MFA response");
          }
        } else {
          std::string error = "MFA failed: " + std::to_string(resp.statusCode);
          rapidjson::Document doc;
          doc.Parse(resp.body.c_str());
          if (!doc.HasParseError() && doc.HasMember("message") &&
              doc["message"].IsString()) {
            error = doc["message"].GetString();
          }
          if (cb)
            cb(false, "", false, "", error);
        }
      },
      {{"Content-Type", "application/json"}});
}

void DiscordClient::sendLazyRequest(const std::string &guildId,
                                    const std::string &channelId) {
  if (guildId.empty() || channelId.empty())
    return;

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
  Logger::log("[Gateway] Sent Lazy Request (Op 14) for Guild %s Channel %s",
              guildId.c_str(), channelId.c_str());
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
            Logger::log("[API] Successfully updated global status to %s",
                        statusStr.c_str());
          } else {
            Logger::log("[API] Failed to update global status: %d %s",
                        resp.statusCode, resp.error.c_str());
          }
        },
        {{"Authorization", token}});
  }

  std::lock_guard<std::recursive_mutex> lock(clientMutex);
  currentUser.status = status;
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
          uint64_t perms = computeChannelPermissions(
              guild, channel, currentUser.id, guild.myRoles);

          bool canSend = (perms & Permissions::SEND_MESSAGES) != 0 ||
                         (perms & Permissions::ADMINISTRATOR) != 0;

          return canSend;
        }
      }

      return false;
    }
  }

  return false;
}

void DiscordClient::parseGuildObject(const rapidjson::Value &gObj, Guild &guild,
                                     const std::string &userId) {
  guild.id = Utils::Json::getString(gObj, "id");
  guild.name = Utils::Json::getString(gObj, "name");
  guild.icon = Utils::Json::getString(gObj, "icon");
  guild.ownerId = Utils::Json::getString(gObj, "owner_id");
  guild.description = Utils::Json::getString(gObj, "description");
  guild.approximateMemberCount =
      Utils::Json::getInt(gObj, "approximate_member_count");
  guild.approximatePresenceCount =
      Utils::Json::getInt(gObj, "approximate_presence_count");
  if (guild.approximateMemberCount == 0) {
    guild.approximateMemberCount = Utils::Json::getInt(gObj, "member_count");
  }

  if (gObj.HasMember("roles") && gObj["roles"].IsArray()) {
    const rapidjson::Value &rolesArr = gObj["roles"];
    guild.roles.clear();
    for (rapidjson::SizeType r = 0; r < rolesArr.Size(); r++) {
      const rapidjson::Value &roleObj = rolesArr[r];
      Role role;
      role.id = Utils::Json::getString(roleObj, "id");
      role.name = Utils::Json::getString(roleObj, "name");
      role.color = Utils::Json::getInt(roleObj, "color");
      role.position = Utils::Json::getInt(roleObj, "position");
      role.permissions = Utils::Json::getUint64(roleObj, "permissions");
      guild.roles.push_back(std::move(role));
    }
  }

  if (gObj.HasMember("members") && gObj["members"].IsArray()) {
    const rapidjson::Value &members = gObj["members"];
    guild.members.clear();
    for (rapidjson::SizeType m = 0; m < members.Size(); m++) {
      const rapidjson::Value &memberObj = members[m];
      if (memberObj.HasMember("user") && memberObj["user"].IsObject()) {
        std::string memberId = Utils::Json::getString(memberObj["user"], "id");

        Member member;
        member.user_id = memberId;
        member.nickname = Utils::Json::getString(memberObj, "nick");

        if (memberObj.HasMember("roles") && memberObj["roles"].IsArray()) {
          const rapidjson::Value &roleIds = memberObj["roles"];
          for (rapidjson::SizeType r = 0; r < roleIds.Size(); r++) {
            if (roleIds[r].IsString()) {
              member.role_ids.push_back(roleIds[r].GetString());
            }
          }
        }

        guild.members.push_back(std::move(member));

        if (memberId == userId) {
          guild.myRoles = member.role_ids;
        }
      }
    }
  }

  if (gObj.HasMember("channels") && gObj["channels"].IsArray()) {
    const rapidjson::Value &channels = gObj["channels"];
    guild.channels.clear();

    for (rapidjson::SizeType k = 0; k < channels.Size(); k++) {
      Channel channel;
      parseChannelObject(channels[k], channel);
      guild.channels.push_back(std::move(channel));
    }

    for (auto &channel : guild.channels) {
      uint64_t finalPerms =
          computeChannelPermissions(guild, channel, userId, guild.myRoles);
      channel.viewable = (finalPerms & Permissions::VIEW_CHANNEL) != 0;
    }
  }
}

void DiscordClient::parseChannelObject(const rapidjson::Value &cObj,
                                       Channel &channel) {
  channel.id = Utils::Json::getString(cObj, "id");
  channel.name = Utils::Json::getString(cObj, "name");
  channel.type = Utils::Json::getInt(cObj, "type");
  channel.last_message_id = Utils::Json::getString(cObj, "last_message_id");
  channel.parent_id = Utils::Json::getString(cObj, "parent_id");
  channel.position = Utils::Json::getInt(cObj, "position");
  channel.topic = Utils::Json::getString(cObj, "topic");
  channel.flags = Utils::Json::getInt(cObj, "flags");

  if (cObj.HasMember("permission_overwrites") &&
      cObj["permission_overwrites"].IsArray()) {
    parseOverwrites(cObj["permission_overwrites"],
                    channel.permission_overwrites);
  }

  if (cObj.HasMember("recipients") && cObj["recipients"].IsArray()) {
    const rapidjson::Value &recipients = cObj["recipients"];
    std::string generatedName;
    for (rapidjson::SizeType r = 0; r < recipients.Size(); r++) {
      const rapidjson::Value &userVal = recipients[r];
      User u;
      u.id = Utils::Json::getString(userVal, "id");
      u.username = Utils::Json::getString(userVal, "username");
      u.global_name = Utils::Json::getString(userVal, "global_name");
      u.avatar = Utils::Json::getString(userVal, "avatar");
      u.discriminator = Utils::Json::getString(userVal, "discriminator");
      channel.recipients.push_back(std::move(u));

      if (!generatedName.empty())
        generatedName += ", ";
      generatedName += u.global_name.empty() ? u.username : u.global_name;
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

void DiscordClient::parseOverwrites(const rapidjson::Value &ows,
                                    std::vector<Overwrite> &overwrites) {
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

} // namespace Discord
