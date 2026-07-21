#ifndef SERVER_LIST_SCREEN_H
#define SERVER_LIST_SCREEN_H

#include "discord/types.h"
#include "screen_manager.h"
#include <map>
#include <string>
#include <unordered_map>
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
		bool hasUnread = false;
		int mentionCount = 0;
	};

	struct ChannelUnreadState {
		bool isUnread = false;
		int mentionCount = 0;
		bool muted = false;
	};

	std::vector<ListItem> listItems;

	void rebuildList();
	const Discord::Guild *getGuild(const std::string &id);
	ListItem createGuildItem(const Discord::Guild *g, int depth);
	ListItem createFolderItem(const Discord::GuildFolder &f);

	void drawListItem(int index, const ListItem &item, float x, float y);

	static const int CHANNEL_ROWS_PER_PAGE = 9;
	int channelRowSpan(int index) const;
	void ensureChannelVisible();

	void drawVoiceStatus();

	std::string voiceNamesResolvedFor;

	int repeatTimer;
	u32 lastKey;
	static const int REPEAT_DELAY_INITIAL = 30;
	static const int REPEAT_DELAY_CONTINUOUS = 6;
	int unreadCacheTimer = 0;
	static const int UNREAD_CACHE_INTERVAL = 60;

	std::vector<Discord::Channel> sortedChannels;
	std::unordered_map<std::string, ChannelUnreadState> channelUnreadCache;
	int channelScrollOffset;
	int selectedChannelIndex;
	void refreshChannels();
	void updateUnreadCache();
	void drawChannelList(float x, float y, float alpha);

	enum class State { SELECTING_SERVER, TRANSITION_TO_CHANNEL, SELECTING_CHANNEL, TRANSITION_TO_SERVER };
	State state;
	float animationProgress;
	float loadingAngle;
	float animTimer;
	static constexpr float SIDEBAR_WIDTH = 72.0f;

	float lerp(float a, float b, float t) { return a + (b - a) * t; }

	bool isMuteMenuOpen = false;
	bool muteMenuIsChannel = false;
	int muteMenuLevel = 0; // 0=top, 1=submenu
	int muteMenuParentIdx = -1;
	int muteMenuIndex = 0;
	std::vector<std::string> muteMenuOptions;
	std::vector<int> muteMenuTimeWindows;
	std::vector<std::string> muteMenuL0Options;
	std::vector<int> muteMenuL0Windows;
	// Level 0 sentinels: INT_MIN=cancel, INT_MIN+1=open duration, INT_MIN+2=mark read,
	//                    INT_MIN+3=open notification levels, 0=unmute
	// Duration submenu: INT_MIN=back, -1=indefinite, >0=minutes
	// Level submenu:    INT_MIN=back, 0..3=message_notifications
	bool muteMenuIsLevelSubmenu = false;
	int muteMenuCheckedIdx = -1;
	std::string muteMenuTargetId;
	bool muteMenuHasUnread = false;
	bool muteMenuIsVoice = false;
	std::string muteMenuExpireLabel; // format: "M/D HH:MM" or empty
	float muteMenuAnchorX = 76.0f;
	float muteMenuAnchorY = 120.0f;
	float muteMenuW0 = 110.0f;
	float muteMenuW1 = 90.0f;
	void buildTopLevelMenu(bool currentlyMuted, bool hasUnread = false);
	void buildDurationMenu();
	void buildLevelMenu();
	void restoreTopLevelMenu();
	void openMuteMenu(const std::string &guildId);
	void openChannelMuteMenu(const std::string &channelId, int channelType);
	void drawMuteMenu();
};

} // namespace UI

#endif // SERVER_LIST_SCREEN_H
