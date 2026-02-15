#ifndef SERVER_LIST_SCREEN_H
#define SERVER_LIST_SCREEN_H

#include "discord/discord_client.h"
#include "screen_manager.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace UI {

class ServerListScreen : public Screen {
public:
  ServerListScreen();
  ~ServerListScreen() override = default;

  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;

  void resetToServerView();

private:
  int selectedIndex;
  int scrollOffset;

  struct ListItem {
    bool isFolder = false;
    std::string id;
    std::string name;
    std::string icon;
    int color = 0;
    std::vector<std::string> folderGuildIds;
    int depth = 0;
    bool expanded = false;
  };

  std::vector<ListItem> listItems;

  void rebuildList();
  const Discord::Guild *getGuild(const std::string &id);
  ListItem createGuildItem(const Discord::Guild *g, int depth);
  ListItem createFolderItem(const Discord::GuildFolder &f);

  void drawListItem(int index, const ListItem &item, float x, float y);

  int repeatTimer;
  u32 lastKey;
  static const int REPEAT_DELAY_INITIAL = 30;
  static const int REPEAT_DELAY_CONTINUOUS = 6;

  std::vector<Discord::Channel> sortedChannels;
  int channelScrollOffset;
  int selectedChannelIndex;
  void refreshChannels();
  void drawChannelList(float x, float y, float alpha);

  std::map<std::string, C3D_Tex *> iconCache;

  enum class State {
    SELECTING_SERVER,
    TRANSITION_TO_CHANNEL,
    SELECTING_CHANNEL,
    TRANSITION_TO_SERVER
  };
  State state;
  float animationProgress;
  float loadingAngle;
  float animTimer;
  static constexpr float SIDEBAR_WIDTH = 72.0f;

  float lerp(float a, float b, float t) { return a + (b - a) * t; }
};

} // namespace UI

#endif // SERVER_LIST_SCREEN_H
