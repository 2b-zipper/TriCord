#ifndef DISCORD_CLIENT_H
#define DISCORD_CLIENT_H

#include "discord/types.h"
#include "network/websocket_client.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <thread>
#include <vector>

namespace Discord {

enum class ConnectionState {
  DISCONNECTED,
  CONNECTING,
  CONNECTED_WS,
  IDENTIFYING,
  AUTHENTICATING,
  READY,
  RECONNECTING,
  DISCONNECTED_ERROR
};

using MessagesCallback = std::function<void(const std::vector<Message> &)>;
using SingleMessageCallback =
    std::function<void(const std::optional<Message> &)>;
using SuccessCallback = std::function<void(bool success)>;
using ThreadsCallback = std::function<void(const std::vector<Channel> &)>;
using TokenCallback = std::function<void(const std::string &token)>;
using MemberCallback = std::function<void(const Member &)>;
using SendMessageCallback =
    std::function<void(const Message &msg, bool success, int code)>;
using ReactionCallback = std::function<void(
    const std::string &channelId, const std::string &messageId,
    const std::string &userId, const Emoji &emoji)>;
using LoginCallback =
    std::function<void(bool success, const std::string &token, bool mfaRequired,
                       const std::string &ticket, const std::string &error)>;

struct TypingUser {
  std::string userId;
  std::string channelId;
  time_t timestamp;
  std::string displayName;
};

class DiscordClient {
public:
  static DiscordClient &getInstance();

  void init();
  void update();
  void shutdown();

  void login(const std::string &token);
  void logout();

  bool connect(const std::string &token);
  void disconnect();

  bool isConnected() const {
    return state != ConnectionState::DISCONNECTED &&
           state != ConnectionState::DISCONNECTED_ERROR;
  }
  bool isReady() const { return state == ConnectionState::READY; }
  ConnectionState getState() const { return state; }

  std::recursive_mutex &getMutex() { return clientMutex; }
  std::string getStatusMessage() {
    std::lock_guard<std::mutex> lock(statusMutex);
    return statusMessage;
  }

  void setMessageCallback(std::function<void(const Message &)> cb) {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    messageCallback = cb;
  }
  void setMessageUpdateCallback(std::function<void(const Message &)> cb) {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    messageUpdateCallback = cb;
  }
  void setMessageDeleteCallback(std::function<void(const std::string &)> cb) {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    messageDeleteCallback = cb;
  }
  void setMessageReactionAddCallback(ReactionCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    messageReactionAddCallback = cb;
  }
  void setMessageReactionRemoveCallback(ReactionCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    messageReactionRemoveCallback = cb;
  }
  void setConnectionCallback(std::function<void()> cb) {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    connectionCallback = cb;
  }

  User getCurrentUser() {
    std::lock_guard<std::recursive_mutex> lock(clientMutex);
    return currentUser;
  }

  const User &getSelf() const { return self; }
  const std::vector<Guild> &getGuilds() { return guilds; }
  const std::vector<GuildFolder> &getGuildFolders() { return folders; }
  const std::vector<Channel> &getPrivateChannels() { return privateChannels; }

  void setSelectedGuildId(const std::string &id) { selectedGuildId = id; }
  std::string getSelectedGuildId() const { return selectedGuildId; }

  void setSelectedChannelId(const std::string &id) { selectedChannelId = id; }
  std::string getSelectedChannelId() const { return selectedChannelId; }

  void fetchMessagesAsync(const std::string &channelId, int limit,
                          MessagesCallback cb, const std::string &around = "");
  void fetchMessagesBeforeAsync(const std::string &channelId,
                                const std::string &beforeId, int limit,
                                MessagesCallback cb);
  void fetchMessage(const std::string &channelId, const std::string &messageId,
                    SingleMessageCallback cb);
  void sendMessage(const std::string &channelId, const std::string &content);
  void sendMessage(const std::string &channelId, const std::string &content,
                   SendMessageCallback cb);
  void sendMessageAsync(const std::string &channelId,
                        const std::string &content, SuccessCallback cb);
  void sendReply(const std::string &channelId, const std::string &content,
                 const std::string &replyId, SendMessageCallback cb);
  bool editMessage(const std::string &channelId, const std::string &messageId,
                   const std::string &content);
  void editMessageAsync(const std::string &channelId,
                        const std::string &messageId,
                        const std::string &content, SuccessCallback cb);
  bool deleteMessage(const std::string &channelId,
                     const std::string &messageId);
  void deleteMessageAsync(const std::string &channelId,
                          const std::string &messageId, SuccessCallback cb);
  void fetchForumThreads(const std::string &channelId, ThreadsCallback cb);
  void exchangeTicketForToken(const std::string &ticket, TokenCallback cb);
  void fetchMember(const std::string &guildId, const std::string &userId,
                   MemberCallback cb);

  void triggerTypingIndicator(const std::string &channelId);
  std::vector<TypingUser> getTypingUsers(const std::string &channelId);

  void performLogin(const std::string &email, const std::string &password,
                    LoginCallback cb);
  void submitMFA(const std::string &ticket, const std::string &code,
                 LoginCallback cb);

  void sendLazyRequest(const std::string &guildId,
                       const std::string &channelId);

  bool canSendMessage(const std::string &channelId);
  void updatePresence(UserStatus status);

  Channel getChannel(const std::string &channelId);
  Member getMember(const std::string &guildId, const std::string &userId);
  int getRoleColor(const std::string &guildId, const Member &member);
  int getRoleColor(const std::string &guildId, const std::string &userId);
  std::string getMemberDisplayName(const std::string &guildId,
                                   const std::string &userId, const User &user);
  std::string getGuildIdFromChannel(const std::string &channelId);

  std::vector<Message> parseMessages(const std::string &json);
  Message parseSingleMessage(const rapidjson::Value &d);
  Message parseSingleMessage(const std::string &json);

  uint64_t calcBasePermissions(const Guild &guild, const std::string &userId,
                               const std::vector<std::string> &memberRoleIds);
  uint64_t
  computeChannelPermissions(const Guild &guild, const Channel &channel,
                            const std::string &userId,
                            const std::vector<std::string> &memberRoleIds);
  uint64_t computeOverwrites(uint64_t base, const std::string &guildId,
                             const std::string &userId,
                             const std::vector<std::string> &memberRoleIds,
                             const rapidjson::Value &overwrites);
  uint64_t computeOverwrites(uint64_t base, const std::string &guildId,
                             const std::string &userId,
                             const std::vector<std::string> &memberRoleIds,
                             const std::vector<Overwrite> &overwrites);

private:
  DiscordClient();
  ~DiscordClient();

  void workerLoop();
  void sendHeartbeat();
  void sendIdentify();
  void sendResume();
  void queueSend(const std::string &message);
  void runNetworkThread(const std::string &token);

  void handleMessage(std::string &message);
  void processMessage(std::string &message);
  void handleHello(const rapidjson::Document &doc);
  void handleDispatch(const rapidjson::Document &doc);
  void handleInvalidSession(const rapidjson::Document &doc);
  void handleReconnect();

  void handleReady(const rapidjson::Value &d);
  void handleResumed();
  void handleGuildCreate(const rapidjson::Value &d);
  void handleChannelCreateUpdate(const rapidjson::Value &d);
  void handleChannelDelete(const rapidjson::Value &d);
  void handleTypingStart(const rapidjson::Value &d);
  void handleMessageCreate(const rapidjson::Value &d);
  void handleMessageUpdate(const rapidjson::Value &d);
  void handleMessageDelete(const rapidjson::Value &d);
  void handleReactionAdd(const rapidjson::Value &d);
  void handleReactionRemove(const rapidjson::Value &d);
  void handlePresenceUpdate(const rapidjson::Value &d);
  void handleUserSettingsUpdate(const rapidjson::Value &d);
  void handleSessionsReplace(const rapidjson::Value &d);
  void parseGuildObject(const rapidjson::Value &gObj, Guild &guild,
                        const std::string &userId);
  void parseChannelObject(const rapidjson::Value &cObj, Channel &channel);
  void parseOverwrites(const rapidjson::Value &ows,
                       std::vector<Overwrite> &overwrites);

  void setState(ConnectionState newState, const std::string &message = "");
  void setStatus(const std::string &message);

  User self;
  User currentUser;
  std::vector<Guild> guilds;
  std::vector<Channel> privateChannels;
  std::vector<GuildFolder> folders;
  std::map<std::string, std::string> channelToGuildCache;

  std::string token;
  ConnectionState state;

  int heartbeatInterval;
  uint64_t lastHeartbeat;
  bool waitingForHeartbeatAck;
  bool hasReceivedHello;
  std::string sessionId;
  int lastSequence;

  bool isConnecting;
  bool stopWorker;
  std::thread workerThread;
  std::thread networkThread;

  std::deque<std::string> messageQueue;
  std::mutex queueMutex;
  std::condition_variable queueCv;

  std::deque<std::string> sendQueue;
  std::mutex sendQueueMutex;

  std::string statusMessage;
  std::mutex statusMutex;

  std::string selectedGuildId;
  std::string selectedChannelId;

  Network::WebSocketClient ws;

  mutable std::recursive_mutex clientMutex;

  std::function<void()> connectionCallback;
  std::function<void(const Message &)> messageCallback;
  std::function<void(const Message &)> messageUpdateCallback;
  std::function<void(const std::string &)> messageDeleteCallback;
  ReactionCallback messageReactionAddCallback;
  ReactionCallback messageReactionRemoveCallback;

  std::map<std::string, std::vector<TypingUser>> typingUsers;
};

} // namespace Discord

#endif // DISCORD_CLIENT_H
