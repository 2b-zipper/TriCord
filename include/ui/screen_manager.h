#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include "ui/hamburger_menu.h"
#include <citro2d.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace UI {

class Screen;

enum class ScreenType {
  LOGIN,
  GUILD_LIST,
  MESSAGES,
  ADD_ACCOUNT,
  FORUM_CHANNEL,
  SETTINGS,
  DM_LIST
};

class Screen {
public:
  Screen();
  virtual ~Screen() = default;

  virtual void update() = 0;
  virtual void renderTop(C3D_RenderTarget *target) = 0;
  virtual void renderBottom(C3D_RenderTarget *target) = 0;
  virtual void onEnter() {}
  virtual void onExit() {}

  bool shouldExit() const { return exitRequested; }

protected:
  bool exitRequested;
};

class ScreenManager {
public:
  static ScreenManager &getInstance();

  void init();
  void shutdown();

  void setScreen(ScreenType type);
  ScreenType getCurrentType() const { return currentType; }
  ScreenType getPreviousType() const { return previousType; }
  void returnToPreviousScreen();
  void update();
  void render();
  void showToast(const std::string &message);

  bool shouldCloseApplication() const { return appExitRequested; }

  void renderDebugOverlay();
  bool isDebugOverlayEnabled() const { return debugOverlayEnabled; }
  void toggleDebugOverlay();

  HamburgerMenu &getHamburgerMenu() { return hamburgerMenu; }

  void setSelectedGuildId(const std::string &id) { selectedGuildId = id; }
  std::string getSelectedGuildId() const { return selectedGuildId; }

  static u32 colorBackground();
  static u32 colorBackgroundDark();
  static u32 colorBackgroundLight();
  static u32 colorPrimary();
  static u32 colorText();
  static u32 colorTextMuted();
  static u32 colorSuccess();
  static u32 colorError();
  static u32 colorInput();
  static u32 colorBoost();
  static u32 colorLink();

  static u32 colorSeparator();
  static u32 colorHeaderGlass();
  static u32 colorHeaderBorder();
  static u32 colorSelection();
  static u32 colorOverlay();
  static u32 colorWhite();
  static u32 colorEmbed();
  static u32 colorEmbedMedia();
  static u32 colorReaction();
  static u32 colorReactionMe();

  int getLastServerIndex() const { return lastServerIndex; }
  void setLastServerIndex(int idx) { lastServerIndex = idx; }
  int getLastServerScroll() const { return lastServerScroll; }
  void setLastServerScroll(int scroll) { lastServerScroll = scroll; }

  void resetSelection();
  void clearCaches();

  int getLastChannelIndex(const std::string &guildId) {
    return lastChannelIndex.count(guildId) ? lastChannelIndex[guildId] : 0;
  }
  void setLastChannelIndex(const std::string &guildId, int idx) {
    lastChannelIndex[guildId] = idx;
  }
  int getLastChannelScroll(const std::string &guildId) {
    return lastChannelScroll.count(guildId) ? lastChannelScroll[guildId] : 0;
  }
  void setLastChannelScroll(const std::string &guildId, int scroll) {
    lastChannelScroll[guildId] = scroll;
  }

  int getLastForumIndex(const std::string &channelId) {
    return lastForumIndex.count(channelId) ? lastForumIndex[channelId] : 0;
  }
  void setLastForumIndex(const std::string &channelId, int idx) {
    lastForumIndex[channelId] = idx;
  }
  int getLastForumScroll(const std::string &channelId) {
    return lastForumScroll.count(channelId) ? lastForumScroll[channelId] : 0;
  }
  void setLastForumScroll(const std::string &channelId, int scroll) {
    lastForumScroll[channelId] = scroll;
  }

  void setFolderExpanded(const std::string &id, bool expanded) {
    if (expanded)
      expandedFolders.insert(id);
    else
      expandedFolders.erase(id);
  }
  bool isFolderExpanded(const std::string &id) {
    return expandedFolders.find(id) != expandedFolders.end();
  }

private:
  ScreenManager()
      : currentType(ScreenType::LOGIN), debugOverlayEnabled(false),
        appExitRequested(false) {}
  ~ScreenManager() = default;

  C3D_RenderTarget *topTarget;
  C3D_RenderTarget *bottomTarget;

  std::unique_ptr<Screen> currentScreen;
  ScreenType currentType;
  ScreenType previousType;
  std::string selectedGuildId;
  bool debugOverlayEnabled;
  bool appExitRequested;
  HamburgerMenu hamburgerMenu;
  C2D_ImageTint tint;

  int lastServerIndex = 0;
  int lastServerScroll = 0;
  std::map<std::string, int> lastChannelIndex;
  std::map<std::string, int> lastChannelScroll;
  std::map<std::string, int> lastForumIndex;
  std::map<std::string, int> lastForumScroll;

  std::set<std::string> expandedFolders;

  void drawHamburgerButton();
  void drawToast();

  std::string toastMessage;
  int toastTimer = 0;
};

void drawText(float x, float y, float z, float scaleX, float scaleY, u32 color,
              const std::string &text);
void drawCenteredText(float y, float z, float scaleX, float scaleY, u32 color,
                      const std::string &text, float screenWidth);
float measureText(const std::string &text, float scaleX, float scaleY);
float measureTextDirect(const std::string &text, float scaleX, float scaleY);
void drawRoundedRect(float x, float y, float z, float w, float h, float radius,
                     u32 color);
void drawCircle(float x, float y, float z, float radius, u32 color);
void drawRichText(float x, float y, float z, float scaleX, float scaleY,
                  u32 color, const std::string &rawText);
void drawCenteredRichText(float y, float z, float scaleX, float scaleY,
                          u32 color, const std::string &rawText,
                          float screenWidth);
float measureRichText(const std::string &rawText, float scaleX, float scaleY);
void drawRichTextUnicodeOnly(float x, float y, float z, float scaleX,
                             float scaleY, u32 color,
                             const std::string &rawText);
float measureRichTextUnicodeOnly(const std::string &rawText, float scaleX,
                                 float scaleY);

std::string getTruncatedText(const std::string &text, float maxWidth,
                             float scaleX, float scaleY);
std::string getTruncatedRichText(const std::string &rawText, float maxWidth,
                                 float scaleX, float scaleY);

void drawOverlay(float z);
void drawPopupBackground(float x, float y, float w, float h, float z,
                         float radius = 12.0f);
void drawPopupMenuItem(float x, float y, float w, float h, float z,
                       bool isSelected, u32 selectionColor);

} // namespace UI

#endif // SCREEN_MANAGER_H
