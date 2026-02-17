#ifndef FORUM_SCREEN_H
#define FORUM_SCREEN_H

#include "discord/discord_client.h"
#include "discord/types.h"
#include "ui/screen_manager.h"
#include <string>
#include <vector>

namespace UI {

class ForumScreen : public Screen {
public:
  ForumScreen(const std::string &channelId, const std::string &channelName);
  ~ForumScreen();

  void onEnter() override;
  void onExit() override;
  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;

private:
  std::string channelId;
  std::string channelName;
  std::string truncatedChannelName;
  std::string channelTopic;
  std::string guildId;
  struct ThreadInfo {
    Discord::Channel channel;
    mutable std::string truncatedTitle;
    mutable std::string truncatedPreview;
    mutable bool titleProcessed = false;
    mutable bool previewProcessed = false;
  };
  std::vector<ThreadInfo> threads;
  int activeThreadCount;
  int selectedIndex;
  int scrollOffset;

  int repeatTimer;
  u32 lastKey;
  static const int REPEAT_DELAY_INITIAL = 30;
  static const int REPEAT_DELAY_CONTINUOUS = 6;

  bool isLoading;

  void fetchThreads();
  void renderThreadCard(int index, float y);

  static constexpr float CARD_HEIGHT = 80.0f;
  static constexpr float CARD_WIDTH = 360.0f;
  static constexpr float MARGIN = 10.0f;
};

} // namespace UI

#endif // FORUM_SCREEN_H
