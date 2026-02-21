#ifndef MESSAGE_SCREEN_H
#define MESSAGE_SCREEN_H

#include "discord/discord_client.h"
#include "discord/types.h"
#include "ui/screen_manager.h"
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace UI {

class MessageScreen : public Screen {
public:
  MessageScreen(const std::string &channelId, const std::string &channelName);
  virtual ~MessageScreen();

  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;
  void onEnter() override;

private:
  std::string channelId;
  std::string channelName;
  std::string truncatedChannelName;
  int channelType;
  std::string channelTopic;
  std::string guildId;
  std::vector<Discord::Message> messages;
  int selectedIndex;
  std::recursive_mutex messageMutex;
  bool isLoading;
  bool isFetchingHistory;
  bool requestHistoryFetch;
  bool scrollInitialized;
  bool showNewMessageIndicator;
  int newMessageCount;
  bool isForumView;
  bool hasMoreHistory;
  uint32_t lastImageGeneration;

  int keyRepeatTimer;
  static const int REPEAT_INITIAL_DELAY = 25;
  static const int REPEAT_INTERVAL = 8;

  std::vector<float> messagePositions;
  std::vector<float> messageHeights;
  std::unordered_map<size_t, float> embedHeightCache;
  float targetScrollY;
  float currentScrollY;
  float totalContentHeight;

  bool isMenuOpen;
  int menuIndex;
  std::vector<std::string> menuOptions;
  std::vector<std::string> menuActions;
  std::set<std::string> pendingMemberFetches;
  std::map<std::string, uint64_t> failedMemberFetches;
  std::shared_ptr<bool> aliveToken;
  void renderMenu();

  void fetchMessages();
  void fetchOlderMessages();
  float drawMessage(const Discord::Message &msg, float y, float maxWidth,
                    bool isSelected, bool showHeader);
  float drawForumMessage(const Discord::Message &msg, float y, bool isSelected);
  float drawSystemMessage(const Discord::Message &msg, float y, float topMargin,
                          float height);
  float drawReplyPreview(const Discord::Message &msg, float x, float y);
  float drawForwardHeader(const Discord::Message &msg, float x, float y);
  float drawAuthorHeader(const Discord::Message &msg, float x, float y,
                         bool showHeader);
  float drawMessageContent(const Discord::Message &msg, float x, float y);
  float drawAttachments(const Discord::Message &msg, float x, float y, float maxWidth);
  float drawStickers(const Discord::Message &msg, float x, float y, float maxWidth);
  float drawReactions(const Discord::Message &msg, float x, float y,
                      bool isSelected);
  float calculateMessageHeight(const Discord::Message &msg, bool showHeader);
  float calculateMessageHeight(const Discord::Message &msg);
  float calculateEmbedHeight(const Discord::Embed &embed, float maxWidth);
  float renderEmbed(const Discord::Embed &embed, float x, float y,
                    float maxWidth);
  void openKeyboard();
  void showMessageOptions();

  void scrollToBottom();
  void rebuildLayoutCache();
  void ensureSelectionVisible();
  void catchUpMessages();
};

} // namespace UI

#endif // MESSAGE_SCREEN_H
