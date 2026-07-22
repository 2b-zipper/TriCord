#include "ui/server_list_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "log.h"
#include "ui/image_manager.h"
#include "ui/voice_controls.h"
#include "utils/message_utils.h"
#include "utils/utf8_utils.h"
#include <3ds.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <ctime>
#include <unordered_map>
#include <unordered_set>

namespace UI {

namespace {
constexpr float VOICE_BTN_Y = BOTTOM_SCREEN_HEIGHT - VoiceControls::BUTTON_SIZE - 10.0f;
constexpr float VOICE_BTN_X = 320.0f - VoiceControls::WIDTH - 10.0f;
} // namespace

ServerListScreen::ServerListScreen()
    : repeatTimer(0), lastKey(0), animationProgress(0.0f), loadingAngle(0.0f), animTimer(0.0f) {
	Logger::log("ServerListScreen initialized");

	auto &sm = ScreenManager::getInstance();
	selectedIndex = sm.getLastServerIndex();
	scrollOffset = sm.getLastServerScroll();

	std::string guildId = sm.getSelectedGuildId();
	if (!guildId.empty()) {
		selectedChannelIndex = sm.getLastChannelIndex(guildId);
		channelScrollOffset = sm.getLastChannelScroll(guildId);
		state = State::SELECTING_CHANNEL;
		animationProgress = 1.0f;
	} else {
		selectedChannelIndex = 0;
		channelScrollOffset = 0;
		state = State::SELECTING_SERVER;
		animationProgress = 0.0f;
	}

	Logger::log("ServerListScreen: Rebuilding list...");
	rebuildList();
	Logger::log("ServerListScreen: Refreshing channels...");
	refreshChannels();

	if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
		if (!listItems[selectedIndex].isFolder && !listItems[selectedIndex].isDm) {
			Discord::DiscordClient::getInstance().fetchGuildDetails(listItems[selectedIndex].id);
		}
	}

	Logger::log("ServerListScreen: Constructor finished");
}

void ServerListScreen::resetToServerView() {
	state = State::SELECTING_SERVER;
	animationProgress = 0.0f;
	selectedChannelIndex = 0;
	channelScrollOffset = 0;

	ScreenManager::getInstance().setSelectedGuildId("");
}

const Discord::Guild *ServerListScreen::getGuild(const std::string &id) {
	const auto &guilds = Discord::DiscordClient::getInstance().getGuilds();
	for (const auto &g : guilds) {
		if (g.id == id) {
			return &g;
		}
	}
	return nullptr;
}

ServerListScreen::ListItem ServerListScreen::createGuildItem(const Discord::Guild *g, int depth) {
	ListItem item;
	item.isFolder = false;
	item.id = g->id;
	item.name = g->name;
	item.icon = g->icon;
	item.depth = depth;
	return item;
}

ServerListScreen::ListItem ServerListScreen::createFolderItem(const Discord::GuildFolder &f) {
	ListItem item;
	item.isFolder = true;
	item.id = f.id;
	item.name = f.name.empty() ? Core::I18n::getInstance().get("common.folder") : f.name;
	item.color = f.color;
	item.expanded = ScreenManager::getInstance().isFolderExpanded(f.id);
	item.depth = 0;
	item.folderGuildIds = f.guildIds;
	return item;
}

void ServerListScreen::rebuildList() {
	Logger::log("ServerListScreen::rebuildList() start");
	listItems.clear();
	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> lock(client.getMutex());

	const auto &folders = client.getGuildFolders();
	std::unordered_set<std::string> visitedGuilds;

	if (folders.empty()) {
		const auto &guilds = client.getGuilds();
		for (const auto &g : guilds) {
			listItems.push_back(createGuildItem(&g, 0));
		}
	} else {
		for (const auto &f : folders) {
			if (f.id.empty()) {
				for (const auto &gid : f.guildIds) {
					const auto *g = getGuild(gid);
					if (g) {
						listItems.push_back(createGuildItem(g, 0));
						visitedGuilds.insert(g->id);
					}
				}
			} else {
				ListItem folderItem = createFolderItem(f);
				listItems.push_back(folderItem);

				if (folderItem.expanded) {
					for (const auto &gid : f.guildIds) {
						const auto *g = getGuild(gid);
						if (g) {
							listItems.push_back(createGuildItem(g, 1));
							visitedGuilds.insert(g->id);
						}
					}
				} else {
					for (const auto &gid : f.guildIds) {
						visitedGuilds.insert(gid);
					}
				}
			}
		}

		const auto &allGuilds = client.getGuilds();
		std::vector<ListItem> orphans;
		for (const auto &g : allGuilds) {
			if (visitedGuilds.find(g.id) == visitedGuilds.end()) {
				orphans.push_back(createGuildItem(&g, 0));
			}
		}

		if (!orphans.empty()) {
			listItems.insert(listItems.begin(), orphans.begin(), orphans.end());
		}
	}
	ListItem dmItem;
	dmItem.isDm = true;
	dmItem.id = "DM";
	dmItem.name = TR("menu.direct_messages");
	listItems.insert(listItems.begin(), dmItem);

	Logger::log("ServerListScreen::rebuildList() end, items: %d", (int)listItems.size());
	updateUnreadCache();
}

void ServerListScreen::refreshChannels() {
	Logger::log("ServerListScreen::refreshChannels() start");
	sortedChannels.clear();
	if (listItems.empty() || selectedIndex < 0 || selectedIndex >= (int)listItems.size()) {
		Logger::log("ServerListScreen::refreshChannels() quick return");
		return;
	}

	const auto &item = listItems[selectedIndex];
	if (item.isFolder) {
		return;
	}

	if (item.isDm) {
		sortedChannels = Discord::DiscordClient::getInstance().getPrivateChannels();
		std::sort(sortedChannels.begin(), sortedChannels.end(),
		          [](const Discord::Channel &a, const Discord::Channel &b) {
			          if (a.last_message_id.length() != b.last_message_id.length()) {
				          return a.last_message_id.length() > b.last_message_id.length();
			          }
			          return a.last_message_id > b.last_message_id;
		          });
		return;
	}

	const auto *guild = getGuild(item.id);
	if (!guild) {
		return;
	}

	std::vector<Discord::Channel> viewableChannels;
	std::vector<Discord::Channel> categories;

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	const auto &currentUser = client.getCurrentUser();
	bool isOwner = (guild->ownerId == currentUser.id);

	bool showHidden = Config::getInstance().isShowHiddenChannelsEnabled();
	for (const auto &ch : guild->channels) {
		if (ch.type == 4) {
			categories.push_back(ch);
		} else if (ch.viewable || isOwner || showHidden) {
			viewableChannels.push_back(ch);
		}
	}

	auto sortByPosAndType = [](const Discord::Channel &a, const Discord::Channel &b) {
		bool aIsVoice = (a.type == 2 || a.type == 13);
		bool bIsVoice = (b.type == 2 || b.type == 13);

		if (aIsVoice != bIsVoice) {
			return !aIsVoice;
		}

		if (a.position != b.position) {
			return a.position < b.position;
		}

		if (a.id.length() != b.id.length()) {
			return a.id.length() < b.id.length();
		}
		return a.id < b.id;
	};

	std::sort(categories.begin(), categories.end(), [](const Discord::Channel &a, const Discord::Channel &b) {
		if (a.position != b.position) {
			return a.position < b.position;
		}
		if (a.id.length() != b.id.length()) {
			return a.id.length() < b.id.length();
		}
		return a.id < b.id;
	});

	std::sort(viewableChannels.begin(), viewableChannels.end(), sortByPosAndType);

	for (const auto &ch : viewableChannels) {
		if (ch.parent_id.empty()) {
			sortedChannels.push_back(ch);
		}
	}

	for (const auto &cat : categories) {
		bool hasVisibleChildren = false;
		for (const auto &ch : viewableChannels) {
			if (ch.parent_id == cat.id) {
				hasVisibleChildren = true;
				break;
			}
		}

		if (hasVisibleChildren || isOwner) {
			if (hasVisibleChildren) {
				sortedChannels.push_back(cat);
				for (const auto &ch : viewableChannels) {
					if (ch.parent_id == cat.id) {
						sortedChannels.push_back(ch);
					}
				}
			}
		}
	}
	updateUnreadCache();
}

static bool isMuteActive(bool muted, const std::string &endTime) {
	if (!muted) {
		return false;
	}
	if (endTime.empty()) {
		return true;
	}
	time_t muteEnd = MessageUtils::parseISO8601(endTime);
	if (muteEnd == 0) {
		return false;
	}
	return muteEnd > MessageUtils::getUtcNow();
}

static const struct {
	const char *key;
	int minutes;
} kMuteDurations[] = {
    {"server.mute.15min", 15}, {"server.mute.1h", 60},    {"server.mute.3h", 180},
    {"server.mute.8h", 480},   {"server.mute.24h", 1440}, {"server.mute.until_unmuted", -1},
};

static const int MENU_OPEN_DURATION = INT_MIN + 1;
static const int MENU_MARK_READ = INT_MIN + 2;
static const int MENU_OPEN_LEVELS = INT_MIN + 3;
static const int MENU_OPEN_CHAT = INT_MIN + 4;

static bool opensSubmenu(int tw) { return tw == MENU_OPEN_DURATION || tw == MENU_OPEN_LEVELS; }

void ServerListScreen::buildTopLevelMenu(bool currentlyMuted, bool hasUnread) {
	muteMenuOptions.clear();
	muteMenuTimeWindows.clear();
	if (muteMenuIsChannel && muteMenuIsVoice) {
		muteMenuOptions.push_back(TR("channel.menu.open_chat"));
		muteMenuTimeWindows.push_back(MENU_OPEN_CHAT);
	}
	if (currentlyMuted) {
		muteMenuOptions.push_back(TR(muteMenuIsChannel ? "channel.mute.unmute" : "server.mute.unmute"));
		muteMenuTimeWindows.push_back(0);
	} else {
		muteMenuOptions.push_back(TR(muteMenuIsChannel ? "channel.menu.mute" : "server.menu.mute"));
		muteMenuTimeWindows.push_back(MENU_OPEN_DURATION);
	}
	muteMenuOptions.push_back(TR("notify.settings"));
	muteMenuTimeWindows.push_back(MENU_OPEN_LEVELS);
	if (hasUnread) {
		muteMenuOptions.push_back(TR("server.menu.mark_read"));
		muteMenuTimeWindows.push_back(MENU_MARK_READ);
	}
	muteMenuOptions.push_back(TR("common.cancel"));
	muteMenuTimeWindows.push_back(INT_MIN);
	muteMenuL0Options = muteMenuOptions;
	muteMenuL0Windows = muteMenuTimeWindows;

	const float PAD_L = 10.0f, PAD_R = 12.0f, ARROW_W = 14.0f;
	muteMenuW0 = 110.0f;
	for (size_t i = 0; i < muteMenuL0Options.size(); i++) {
		float tw = measureRichText(muteMenuL0Options[i], 0.48f, 0.48f);
		bool hasArrow = opensSubmenu(muteMenuL0Windows[i]);
		float need = PAD_L + tw + (hasArrow ? ARROW_W + 4.0f : PAD_R);
		if (need > muteMenuW0) {
			muteMenuW0 = need;
		}
	}
	muteMenuW0 = std::min(muteMenuW0, 390.0f - muteMenuAnchorX);
}

void ServerListScreen::buildDurationMenu() {
	muteMenuOptions.clear();
	muteMenuTimeWindows.clear();
	for (const auto &d : kMuteDurations) {
		muteMenuOptions.push_back(TR(d.key));
		muteMenuTimeWindows.push_back(d.minutes);
	}
	muteMenuOptions.push_back(TR("server.mute.back"));
	muteMenuTimeWindows.push_back(INT_MIN);

	const float PAD_L = 10.0f, PAD_R = 12.0f;
	muteMenuW1 = 90.0f;
	for (const auto &opt : muteMenuOptions) {
		float need = PAD_L + measureRichText(opt, 0.48f, 0.48f) + PAD_R;
		if (need > muteMenuW1) {
			muteMenuW1 = need;
		}
	}
	muteMenuW1 = std::min(muteMenuW1, 400.0f - (muteMenuAnchorX + muteMenuW0 + 3.0f) - 4.0f);
	muteMenuIsLevelSubmenu = false;
}

void ServerListScreen::buildLevelMenu() {
	muteMenuOptions.clear();
	muteMenuTimeWindows.clear();

	// Mirrors the resolve order so the marker points at the setting actually in effect.
	int current = muteMenuIsChannel ? 3 : 0;
	Discord::DiscordClient &dc = Discord::DiscordClient::getInstance();
	{
		std::lock_guard<std::recursive_mutex> lock(dc.getMutex());
		const auto &notifSettings = dc.getNotificationSettings();
		if (muteMenuIsChannel) {
			auto gsIt = notifSettings.find(dc.getGuildIdFromChannel(muteMenuTargetId));
			if (gsIt != notifSettings.end()) {
				auto coIt = gsIt->second.channelOverrides.find(muteMenuTargetId);
				if (coIt != gsIt->second.channelOverrides.end()) {
					const auto &co = coIt->second;
					if (co.flags & (1 << 10)) {
						current = 0;
					} else if (co.flags & (1 << 9)) {
						current = 1;
					} else {
						current = co.messageNotifications;
					}
				}
			}
		} else {
			auto gsIt = notifSettings.find(muteMenuTargetId);
			if (gsIt != notifSettings.end()) {
				const auto &gs = gsIt->second;
				if (gs.flags & (1 << 11)) {
					current = 0;
				} else if (gs.flags & (1 << 12)) {
					current = 1;
				} else {
					current = (gs.messageNotifications == 3) ? 0 : gs.messageNotifications;
				}
			}
		}
	}

	muteMenuCheckedIdx = -1;
	auto add = [&](const char *key, int value) {
		if (value == current) {
			muteMenuCheckedIdx = (int)muteMenuOptions.size();
		}
		muteMenuOptions.push_back(TR(key));
		muteMenuTimeWindows.push_back(value);
	};

	if (muteMenuIsChannel) {
		add("notify.default", 3);
	}
	add("notify.all", 0);
	add("notify.mentions", 1);
	add("notify.nothing", 2);
	muteMenuOptions.push_back(TR("server.mute.back"));
	muteMenuTimeWindows.push_back(INT_MIN);

	const float PAD_L = 10.0f, PAD_R = 24.0f;
	muteMenuW1 = 90.0f;
	for (const auto &opt : muteMenuOptions) {
		float need = PAD_L + measureRichText(opt, 0.48f, 0.48f) + PAD_R;
		if (need > muteMenuW1) {
			muteMenuW1 = need;
		}
	}
	muteMenuW1 = std::min(muteMenuW1, 400.0f - (muteMenuAnchorX + muteMenuW0 + 3.0f) - 4.0f);
	muteMenuIsLevelSubmenu = true;
}

void ServerListScreen::restoreTopLevelMenu() {
	bool currentlyMuted = false;
	Discord::DiscordClient &dc = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> lock(dc.getMutex());
	const auto &notifSettings = dc.getNotificationSettings();
	if (muteMenuIsChannel) {
		std::string guildId = dc.getGuildIdFromChannel(muteMenuTargetId);
		auto gsIt = notifSettings.find(guildId);
		if (gsIt != notifSettings.end()) {
			auto coIt = gsIt->second.channelOverrides.find(muteMenuTargetId);
			if (coIt != gsIt->second.channelOverrides.end()) {
				currentlyMuted = isMuteActive(coIt->second.muted, coIt->second.muteEndTime);
			}
		}
	} else {
		auto gsIt = notifSettings.find(muteMenuTargetId);
		if (gsIt != notifSettings.end()) {
			currentlyMuted = isMuteActive(gsIt->second.muted, gsIt->second.muteEndTime);
		}
	}
	buildTopLevelMenu(currentlyMuted, muteMenuHasUnread);
	muteMenuLevel = 0;
	muteMenuIndex = (muteMenuParentIdx >= 0) ? muteMenuParentIdx : 0;
	muteMenuParentIdx = -1;
	muteMenuIsLevelSubmenu = false;
}

void ServerListScreen::openMuteMenu(const std::string &guildId) {
	muteMenuTargetId = guildId;
	muteMenuIsChannel = false;
	muteMenuIsVoice = false;
	muteMenuAnchorX = SIDEBAR_WIDTH + 4.0f;
	int rel = selectedIndex - scrollOffset;
	muteMenuAnchorY = (rel >= 0 && rel < 5) ? rel * 48.0f + 24.0f : 120.0f;

	Discord::DiscordClient &dc = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> lock(dc.getMutex());
	const auto &notifSettings = dc.getNotificationSettings();

	bool currentlyMuted = false;
	muteMenuExpireLabel.clear();
	auto gsIt = notifSettings.find(guildId);
	if (gsIt != notifSettings.end()) {
		currentlyMuted = isMuteActive(gsIt->second.muted, gsIt->second.muteEndTime);
		if (currentlyMuted && !gsIt->second.muteEndTime.empty()) {
			struct tm tm_local;
			if (MessageUtils::getLocalTm(gsIt->second.muteEndTime, tm_local)) {
				char buf[32];
				snprintf(buf, sizeof(buf), "%d/%d %02d:%02d", tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour,
				         tm_local.tm_min);
				muteMenuExpireLabel = buf;
			}
		}
	}

	bool hasUnread = false;
	for (const auto &guild : dc.getGuilds()) {
		if (guild.id != guildId) {
			continue;
		}
		for (const auto &ch : guild.channels) {
			auto cit = channelUnreadCache.find(ch.id);
			if (cit != channelUnreadCache.end() && cit->second.isUnread) {
				hasUnread = true;
				break;
			}
		}
		break;
	}

	muteMenuHasUnread = hasUnread;
	buildTopLevelMenu(currentlyMuted, hasUnread);
	muteMenuLevel = 0;
	muteMenuIndex = 0;
	muteMenuParentIdx = -1;
	muteMenuIsLevelSubmenu = false;
	isMuteMenuOpen = true;
}

void ServerListScreen::openChannelMuteMenu(const std::string &channelId, int channelType) {
	muteMenuTargetId = channelId;
	muteMenuIsChannel = true;
	muteMenuIsVoice = channelType == 2 || channelType == 13;
	muteMenuAnchorX = 4.0f;
	int rel = selectedChannelIndex - channelScrollOffset;
	muteMenuAnchorY = (rel >= 0 && rel < 9) ? 26.0f + rel * 22.0f + 11.0f : 120.0f;

	Discord::DiscordClient &dc = Discord::DiscordClient::getInstance();
	std::string guildId = dc.getGuildIdFromChannel(channelId);
	std::lock_guard<std::recursive_mutex> lock(dc.getMutex());
	const auto &notifSettings = dc.getNotificationSettings();

	bool currentlyMuted = false;
	muteMenuExpireLabel.clear();
	auto gsIt = notifSettings.find(guildId);
	if (gsIt != notifSettings.end()) {
		auto coIt = gsIt->second.channelOverrides.find(channelId);
		if (coIt != gsIt->second.channelOverrides.end()) {
			currentlyMuted = isMuteActive(coIt->second.muted, coIt->second.muteEndTime);
			if (currentlyMuted && !coIt->second.muteEndTime.empty()) {
				struct tm tm_local;
				if (MessageUtils::getLocalTm(coIt->second.muteEndTime, tm_local)) {
					char buf[32];
					snprintf(buf, sizeof(buf), "%d/%d %02d:%02d", tm_local.tm_mon + 1, tm_local.tm_mday,
					         tm_local.tm_hour, tm_local.tm_min);
					muteMenuExpireLabel = buf;
				}
			}
		}
	}

	bool hasUnread = false;
	{
		const auto &readStates = dc.getReadStates();
		for (const auto &guild : dc.getGuilds()) {
			if (guild.id != guildId) {
				continue;
			}
			for (const auto &ch : guild.channels) {
				if (ch.id != channelId) {
					continue;
				}
				if (!ch.last_message_id.empty()) {
					auto rsIt = readStates.find(channelId);
					if (rsIt == readStates.end() || rsIt->second.lastReadMessageId.empty() ||
					    MessageUtils::isNewerSnowflake(ch.last_message_id, rsIt->second.lastReadMessageId)) {
						hasUnread = true;
					}
				}
				break;
			}
			break;
		}
	}
	muteMenuHasUnread = hasUnread;
	buildTopLevelMenu(currentlyMuted, hasUnread);
	muteMenuLevel = 0;
	muteMenuIndex = 0;
	muteMenuParentIdx = -1;
	muteMenuIsLevelSubmenu = false;
	isMuteMenuOpen = true;
}

void ServerListScreen::drawMuteMenu() {
	if (muteMenuL0Options.empty()) {
		return;
	}
	drawOverlay(0.98f);

	const float ITEM_H = 22.0f;
	const float PAD_V = 4.0f;
	const float PAD_L = 10.0f;
	const float SEP_H = 1.0f;
	const float GAP = 3.0f;
	const float RADIUS = 6.0f;
	const float W0 = muteMenuW0;

	u32 bg = ScreenManager::colorBackground();
	u32 bgd = ScreenManager::colorBackgroundDark();
	u32 sep = ScreenManager::colorHeaderBorder();
	u32 acc = ScreenManager::colorAccent();
	u32 hl = ScreenManager::colorBackgroundLight();
	u32 txt = ScreenManager::colorText();
	u32 dim = ScreenManager::colorTextMuted();

	const float SUBTITLE_EXTRA = 8.0f;
	bool hasExpireLabel = !muteMenuExpireLabel.empty();
	int n0 = (int)muteMenuL0Options.size();
	float h0 = 2.0f * PAD_V + (float)n0 * ITEM_H + SEP_H + (hasExpireLabel ? SUBTITLE_EXTRA : 0.0f);
	float x0 = muteMenuAnchorX;
	float y0 = muteMenuAnchorY - h0 / 2.0f;
	y0 = std::max(4.0f, std::min(y0, 236.0f - h0));

	int parentIdx = muteMenuParentIdx;

	// Border outline prevents shadow bleed
	drawRoundedRect(x0 - 1.0f, y0 - 1.0f, 0.989f, W0 + 2.0f, h0 + 2.0f, RADIUS + 1.0f, bgd);
	drawRoundedRect(x0, y0, 0.99f, W0, h0, RADIUS, bg);

	{
		float iy = y0 + PAD_V;
		for (int i = 0; i < n0; i++) {
			bool isLast = (i == n0 - 1);
			if (isLast) {
				C2D_DrawRectSolid(x0 + 4.0f, iy, 0.992f, W0 - 8.0f, SEP_H, sep);
				iy += SEP_H;
			}

			bool isUnmute = (muteMenuL0Windows[i] == 0);
			bool hasSubtitle = isUnmute && hasExpireLabel;
			float itemH = hasSubtitle ? ITEM_H + SUBTITLE_EXTRA : ITEM_H;

			bool sel = (muteMenuLevel == 0) ? (muteMenuIndex == i) : (i == parentIdx);
			if (sel) {
				drawRoundedRect(x0 + 2.0f, iy + 1.0f, 0.993f, W0 - 4.0f, itemH - 2.0f, 4.0f, hl);
				C2D_DrawRectSolid(x0 + 2.0f, iy + 1.0f, 0.994f, 2.5f, itemH - 2.0f, acc);
			}

			u32 color = isLast ? dim : txt;
			float textY = hasSubtitle ? iy + 3.0f : iy + 5.0f;
			drawRichText(x0 + PAD_L, textY, 0.995f, 0.48f, 0.48f, color, muteMenuL0Options[i]);
			if (hasSubtitle) {
				drawText(x0 + PAD_L, iy + 16.0f, 0.995f, 0.38f, 0.38f, dim, muteMenuExpireLabel);
			}

			if (opensSubmenu(muteMenuL0Windows[i])) {
				drawText(x0 + W0 - 14.0f, iy + 5.0f, 0.995f, 0.45f, 0.45f, dim, ">");
			}

			iy += itemH;
		}
	}

	if (muteMenuLevel == 1) {
		int n1 = (int)muteMenuOptions.size();
		const float W1 = muteMenuW1;
		float h1 = 2.0f * PAD_V + (float)n1 * ITEM_H + SEP_H;
		float x1 = x0 + W0 + GAP;
		float parentItemY = y0 + PAD_V + (float)parentIdx * ITEM_H;
		float y1 = std::max(4.0f, std::min(parentItemY, 236.0f - h1));

		drawRoundedRect(x1 - 1.0f, y1 - 1.0f, 0.989f, W1 + 2.0f, h1 + 2.0f, RADIUS + 1.0f, bgd);
		drawRoundedRect(x1, y1, 0.99f, W1, h1, RADIUS, bg);

		float jy = y1 + PAD_V;
		for (int i = 0; i < n1; i++) {
			bool isLast = (i == n1 - 1);
			if (isLast) {
				C2D_DrawRectSolid(x1 + 4.0f, jy, 0.992f, W1 - 8.0f, SEP_H, sep);
				jy += SEP_H;
			}

			bool sel = (muteMenuIndex == i);
			if (sel) {
				drawRoundedRect(x1 + 2.0f, jy + 1.0f, 0.993f, W1 - 4.0f, ITEM_H - 2.0f, 4.0f, hl);
				C2D_DrawRectSolid(x1 + 2.0f, jy + 1.0f, 0.994f, 2.5f, ITEM_H - 2.0f, acc);
			}

			u32 color = isLast ? dim : txt;
			drawRichText(x1 + PAD_L, jy + 5.0f, 0.995f, 0.48f, 0.48f, color, muteMenuOptions[i]);
			if (muteMenuIsLevelSubmenu && i == muteMenuCheckedIdx) {
				drawCircle(x1 + W1 - 12.0f, jy + ITEM_H / 2.0f, 0.995f, 3.0f, acc);
			}
			jy += ITEM_H;
		}
	}
}

void ServerListScreen::updateUnreadCache() {
	Discord::DiscordClient &dc = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> lock(dc.getMutex());

	const auto &readStates = dc.getReadStates();
	const auto &notifSettings = dc.getNotificationSettings();

	std::unordered_map<std::string, std::pair<bool, int>> guildSummary;
	channelUnreadCache.clear();

	for (const auto &guild : dc.getGuilds()) {
		bool guildMuted = false;
		auto gsIt = notifSettings.find(guild.id);
		if (gsIt != notifSettings.end()) {
			guildMuted = isMuteActive(gsIt->second.muted, gsIt->second.muteEndTime);
		}

		bool guildHasUnread = false;
		int guildMentions = 0;

		for (const auto &ch : guild.channels) {
			if (ch.type == 2 || ch.type == 4 || ch.type == 13) {
				continue;
			}
			if (!ch.viewable) {
				continue;
			}
			if (ch.last_message_id.empty()) {
				continue;
			}

			bool chMuted = false;
			if (gsIt != notifSettings.end()) {
				auto coIt = gsIt->second.channelOverrides.find(ch.id);
				if (coIt != gsIt->second.channelOverrides.end()) {
					chMuted = isMuteActive(coIt->second.muted, coIt->second.muteEndTime);
				}
			}

			auto rsIt = readStates.find(ch.id);
			if (rsIt == readStates.end() || rsIt->second.lastReadMessageId.empty()) {
				channelUnreadCache[ch.id] = {false, 0, chMuted};
				continue;
			}

			bool rawUnread = MessageUtils::isNewerSnowflake(ch.last_message_id, rsIt->second.lastReadMessageId);
			int mentions = rsIt->second.mentionCount;

			// Mute hides the unread dot only; mention_count keeps counting while muted.
			bool showUnread = rawUnread && !chMuted;
			int showMentions = mentions;

			channelUnreadCache[ch.id] = {showUnread, showMentions, chMuted};

			if (showUnread) {
				guildHasUnread = true;
			}
			guildMentions += showMentions;
		}

		guildSummary[guild.id] = {guildMuted ? false : guildHasUnread, guildMentions};
	}

	{
		int dmMentions = 0;
		for (const auto &dm : dc.getPrivateChannels()) {
			if (dm.last_message_id.empty()) {
				continue;
			}
			auto rsIt = readStates.find(dm.id);
			if (rsIt == readStates.end() || rsIt->second.lastReadMessageId.empty()) {
				channelUnreadCache[dm.id] = {false, 0, false};
				continue;
			}

			bool unread = MessageUtils::isNewerSnowflake(dm.last_message_id, rsIt->second.lastReadMessageId);
			int mentions = rsIt->second.mentionCount;

			channelUnreadCache[dm.id] = {unread, mentions, false};
			dmMentions += (mentions > 0) ? mentions : (unread ? 1 : 0);
		}
		// Mention badge only; the plain dot is redundant for DMs.
		guildSummary["DM"] = {false, dmMentions};
	}

	for (auto &item : listItems) {
		if (item.isFolder) {
			item.hasUnread = false;
			item.mentionCount = 0;
			for (const auto &gid : item.folderGuildIds) {
				auto it = guildSummary.find(gid);
				if (it != guildSummary.end()) {
					if (it->second.first) {
						item.hasUnread = true;
					}
					item.mentionCount += it->second.second;
				}
			}
		} else {
			auto it = guildSummary.find(item.id);
			if (it != guildSummary.end()) {
				item.hasUnread = it->second.first;
				item.mentionCount = it->second.second;
			} else {
				item.hasUnread = false;
				item.mentionCount = 0;
			}
		}
	}
}

void ServerListScreen::update() {
	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	auto &sm = ScreenManager::getInstance();
	std::lock_guard<std::recursive_mutex> lock(client.getMutex());
	client.update();

	std::string dirtyChannel = client.consumeReadStateDirty();
	bool ackForVisibleChannel = false;
	if (!dirtyChannel.empty()) {
		for (const auto &ch : sortedChannels) {
			if (ch.id == dirtyChannel) {
				ackForVisibleChannel = true;
				break;
			}
		}
	}
	bool guildDataChanged = client.consumeGuildDataDirty();
	if (ackForVisibleChannel || guildDataChanged || ++unreadCacheTimer >= UNREAD_CACHE_INTERVAL) {
		unreadCacheTimer = 0;
		updateUnreadCache();
	}

	for (int i = channelScrollOffset; i < (int)sortedChannels.size() && i < channelScrollOffset + 6; i++) {
		const auto &dm = sortedChannels[i];
		if (dm.type == 1 && !dm.recipients.empty()) {
			const auto &r = dm.recipients[0];
			Discord::AvatarCache::getInstance().prefetchAvatar(r.id, r.avatar, r.discriminator);
		} else if (dm.type == 3 && !dm.icon.empty()) {
			Discord::AvatarCache::getInstance().prefetchChannelIcon(dm.id, dm.icon);
		}
	}

	if (listItems.empty()) {
		if (!client.getGuilds().empty()) {
			rebuildList();
			refreshChannels();
		} else if (client.getState() != Discord::ConnectionState::READY) {
			animTimer += 1.0f / 60.0f;
			if (animTimer >= 1.5f) {
				animTimer = 0.0f;
			}

			float t = 0.0f;
			if (animTimer < 1.0f) {
				float x = animTimer;
				const float c1 = 1.0f;
				const float c3 = c1 + 1.0f;
				float xm1 = x - 1.0f;
				t = 1.0f + c3 * pow(xm1, 3.0f) + c1 * pow(xm1, 2.0f);
			} else {
				t = 1.0f;
			}
			loadingAngle = 360.0f * t;
			return;
		}
	}

	if (listItems.empty()) {
		return;
	}

	if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
		const auto &highlighted = listItems[selectedIndex];
		if (!highlighted.isFolder && highlighted.id != voiceNamesResolvedFor) {
			voiceNamesResolvedFor = highlighted.id;
			client.resolveVoiceNames(highlighted.id);
		}
	}

	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();

	if (kDown & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);
		if (VoiceControls::handleTouch(touch, VOICE_BTN_X, VOICE_BTN_Y)) {
			return;
		}
	}

	if (state == State::TRANSITION_TO_CHANNEL) {
		animationProgress += 0.1f;
		if (animationProgress >= 1.0f) {
			animationProgress = 1.0f;
			state = State::SELECTING_CHANNEL;
		}
		return;
	} else if (state == State::TRANSITION_TO_SERVER) {
		animationProgress -= 0.1f;
		if (animationProgress <= 0.0f) {
			animationProgress = 0.0f;
			state = State::SELECTING_SERVER;
			unreadCacheTimer = UNREAD_CACHE_INTERVAL;
		}
		return;
	}

	if (isMuteMenuOpen) {
		if (kDown & KEY_DOWN && muteMenuIndex < (int)muteMenuOptions.size() - 1) {
			muteMenuIndex++;
		}
		if (kDown & KEY_UP && muteMenuIndex > 0) {
			muteMenuIndex--;
		}
		if (kDown & KEY_B) {
			if (muteMenuLevel == 1) {
				restoreTopLevelMenu();
			} else {
				isMuteMenuOpen = false;
			}
		}
		if (kDown & KEY_A && muteMenuIndex < (int)muteMenuTimeWindows.size()) {
			int tw = muteMenuTimeWindows[muteMenuIndex];
			if (muteMenuLevel == 0) {
				if (tw == INT_MIN) {
					isMuteMenuOpen = false;
				} else if (opensSubmenu(tw)) {
					if (tw == MENU_OPEN_DURATION) {
						buildDurationMenu();
					} else {
						buildLevelMenu();
					}
					muteMenuParentIdx = muteMenuIndex;
					muteMenuLevel = 1;
					muteMenuIndex = 0;
				} else if (tw == MENU_OPEN_CHAT) {
					isMuteMenuOpen = false;
					Discord::DiscordClient::getInstance().setSelectedChannelId(muteMenuTargetId);
					ScreenManager::getInstance().pushScreen(ScreenType::MESSAGES);
				} else if (tw == MENU_MARK_READ) {
					isMuteMenuOpen = false;
					if (muteMenuIsChannel) {
						Discord::DiscordClient::getInstance().markChannelReadLatest(muteMenuTargetId);
					} else {
						Discord::DiscordClient::getInstance().markGuildRead(muteMenuTargetId);
					}
					updateUnreadCache();
				} else {
					isMuteMenuOpen = false;
					if (muteMenuIsChannel) {
						Discord::DiscordClient::getInstance().setChannelMuted(muteMenuTargetId, false, 0);
					} else {
						Discord::DiscordClient::getInstance().setGuildMuted(muteMenuTargetId, false, 0);
					}
					updateUnreadCache();
				}
			} else if (tw == INT_MIN) {
				restoreTopLevelMenu();
			} else if (muteMenuIsLevelSubmenu) {
				isMuteMenuOpen = false;
				if (muteMenuIsChannel) {
					Discord::DiscordClient::getInstance().setChannelNotificationLevel(muteMenuTargetId, tw);
				} else {
					Discord::DiscordClient::getInstance().setGuildNotificationLevel(muteMenuTargetId, tw);
				}
				updateUnreadCache();
			} else {
				isMuteMenuOpen = false;
				bool muting = (tw != 0);
				if (muteMenuIsChannel) {
					Discord::DiscordClient::getInstance().setChannelMuted(muteMenuTargetId, muting, tw);
				} else {
					Discord::DiscordClient::getInstance().setGuildMuted(muteMenuTargetId, muting, tw);
				}
				updateUnreadCache();
			}
		}
		return;
	}

	u32 moveDir = 0;
	if (kDown & KEY_DOWN) {
		moveDir = KEY_DOWN;
	} else if (kDown & KEY_UP) {
		moveDir = KEY_UP;
	} else if (kHeld & KEY_DOWN && (--repeatTimer <= 0)) {
		moveDir = KEY_DOWN;
		repeatTimer = REPEAT_DELAY_CONTINUOUS;
	} else if (kHeld & KEY_UP && (--repeatTimer <= 0)) {
		moveDir = KEY_UP;
		repeatTimer = REPEAT_DELAY_CONTINUOUS;
	}

	if (kDown & (KEY_DOWN | KEY_UP)) {
		repeatTimer = REPEAT_DELAY_INITIAL;
		lastKey = (kDown & KEY_DOWN) ? KEY_DOWN : KEY_UP;
	}
	if (!(kHeld & (KEY_DOWN | KEY_UP))) {
		lastKey = 0;
	}

	if (state == State::SELECTING_SERVER) {
		bool selectionChanged = false;
		if (moveDir & KEY_DOWN) {
			if (selectedIndex < (int)listItems.size() - 1) {
				selectedIndex++;
				if (selectedIndex >= scrollOffset + 5) {
					scrollOffset = selectedIndex - 4;
				}
				selectionChanged = true;
			}
		} else if (moveDir & KEY_UP) {
			if (selectedIndex > 0) {
				selectedIndex--;
				if (selectedIndex < scrollOffset) {
					scrollOffset = selectedIndex;
				}
				selectionChanged = true;
			}
		}

		if (selectionChanged) {
			sm.setLastServerIndex(selectedIndex);
			sm.setLastServerScroll(scrollOffset);
			refreshChannels();
			channelScrollOffset = 0;
			selectedChannelIndex = 0;
			while (selectedChannelIndex < (int)sortedChannels.size() &&
			       sortedChannels[selectedChannelIndex].type == 4) {
				selectedChannelIndex++;
			}
			selectedChannelIndex = -1;

			sm.setLastChannelIndex(sm.getSelectedGuildId(), selectedChannelIndex);
			sm.setLastChannelScroll(sm.getSelectedGuildId(), channelScrollOffset);

			if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
				if (!listItems[selectedIndex].isFolder && !listItems[selectedIndex].isDm) {
					Discord::DiscordClient::getInstance().fetchGuildDetails(listItems[selectedIndex].id);
				}
			}
		}

		if (kDown & KEY_X) {
			if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
				const auto &item = listItems[selectedIndex];
				if (!item.isFolder && !item.isDm) {
					openMuteMenu(item.id);
				}
			}
		}

		if (kDown & KEY_A) {
			if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
				const auto &item = listItems[selectedIndex];
				if (item.isFolder) {
					bool isExpanded = ScreenManager::getInstance().isFolderExpanded(item.id);
					ScreenManager::getInstance().setFolderExpanded(item.id, !isExpanded);
					rebuildList();
					refreshChannels();
				} else {
					state = State::TRANSITION_TO_CHANNEL;
					ScreenManager::getInstance().setSelectedGuildId(item.id);

					selectedChannelIndex = 0;
					while (selectedChannelIndex < (int)sortedChannels.size() &&
					       sortedChannels[selectedChannelIndex].type == 4) {
						selectedChannelIndex++;
					}
					channelScrollOffset = 0;

					sm.setLastChannelIndex(item.id, selectedChannelIndex);
					sm.setLastChannelScroll(item.id, channelScrollOffset);
				}
			}
		}
	} else if (state == State::SELECTING_CHANNEL) {

		if (!sortedChannels.empty()) {
			if (moveDir & KEY_DOWN) {
				if (selectedChannelIndex < (int)sortedChannels.size() - 1) {
					int nextIndex = selectedChannelIndex + 1;
					while (nextIndex < (int)sortedChannels.size() && sortedChannels[nextIndex].type == 4) {
						nextIndex++;
					}
					if (nextIndex < (int)sortedChannels.size()) {
						selectedChannelIndex = nextIndex;
						ensureChannelVisible();
					}
				}
			} else if (moveDir & KEY_UP) {
				if (selectedChannelIndex > 0) {
					int prevIndex = selectedChannelIndex - 1;
					while (prevIndex >= 0 && sortedChannels[prevIndex].type == 4) {
						prevIndex--;
					}
					if (prevIndex >= 0) {
						selectedChannelIndex = prevIndex;
						ensureChannelVisible();
					}
				}
			}

			if (moveDir & (KEY_UP | KEY_DOWN)) {
				sm.setLastChannelIndex(sm.getSelectedGuildId(), selectedChannelIndex);
				sm.setLastChannelScroll(sm.getSelectedGuildId(), channelScrollOffset);
			}
		}

		if (kDown & KEY_Y) {
			Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();
			if (voice.getState() == Discord::VoiceState::ESTABLISHED && !voice.isServerMuted()) {
				voice.setMuted(!voice.isMuted());
			}
		} else if (kDown & KEY_X) {
			if (selectedChannelIndex >= 0 && selectedChannelIndex < (int)sortedChannels.size()) {
				const auto &ch = sortedChannels[selectedChannelIndex];
				// DMs have no entry in the guild notification tree, so the menu has nothing to act on.
				if (ch.type != 4 && ch.type != 1 && ch.type != 3) {
					openChannelMuteMenu(ch.id, ch.type);
				}
			}
		} else if (kDown & KEY_B) {
			state = State::TRANSITION_TO_SERVER;
			sm.setLastChannelIndex(sm.getSelectedGuildId(), -1);
			sm.setLastChannelScroll(sm.getSelectedGuildId(), 0);
		} else if (kDown & KEY_A) {
			if (selectedChannelIndex >= 0 && selectedChannelIndex < (int)sortedChannels.size()) {
				const auto &ch = sortedChannels[selectedChannelIndex];
				if (ch.type == 0 || ch.type == 5 || ch.type == 10 || ch.type == 11 || ch.type == 12 || ch.type == 1 ||
				    ch.type == 3) {
					Discord::DiscordClient::getInstance().setSelectedChannelId(ch.id);
					ScreenManager::getInstance().pushScreen(ScreenType::MESSAGES);
				} else if (ch.type == 15) {
					Discord::DiscordClient::getInstance().setSelectedChannelId(ch.id);
					ScreenManager::getInstance().pushScreen(ScreenType::FORUM_CHANNEL);
				} else if (ch.type == 2 || ch.type == 13) {
					Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();
					if (voice.getState() == Discord::VoiceState::DISCONNECTED ||
					    voice.getState() == Discord::VoiceState::FAILED) {
						voice.connect(sm.getSelectedGuildId(), ch.id);
					} else {
						voice.disconnect();
					}
				}
			}
		}
	}
}

void ServerListScreen::renderTop(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackground());

	if (listItems.empty()) {
		if (Discord::DiscordClient::getInstance().getGuilds().empty() &&
		    Discord::DiscordClient::getInstance().getState() == Discord::ConnectionState::READY) {
			float headerH = 26.0f;
			C2D_DrawRectSolid(0, 0, 0.9f, 400.0f, headerH, ScreenManager::colorHeaderGlass());
			C2D_DrawRectSolid(0, headerH - 1.0f, 0.91f, 400.0f, 1.0f, ScreenManager::colorHeaderBorder());

			drawCenteredRichText(4.0f, 0.95f, 0.52f, 0.52f, ScreenManager::colorText(), TR("menu.servers"), 400.0f);

			drawCenteredText(120.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(),
			                 Core::I18n::getInstance().get("server.no_servers"), 400.0f);
			return;
		}

		float centerX = 200.0f;
		float centerY = 120.0f;

		C3D_Tex *discordTex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord.png", true);
		if (discordTex) {
			UI::ImageManager::ImageInfo info = UI::ImageManager::getInstance().getImageInfo("romfs:/discord.png");
			Tex3DS_SubTexture subtex;
			subtex.width = (u16)info.originalW;
			subtex.height = (u16)info.originalH;
			subtex.left = 0.0f;
			subtex.top = 0.0f;
			subtex.right = (float)info.originalW / discordTex->width;
			subtex.bottom = (float)info.originalH / discordTex->height;
			C2D_Image img = {discordTex, &subtex};
			float scale = 90.0f / (float)info.originalW;
			float rad = (loadingAngle - 90.0f) * M_PI / 180.0f;
			C2D_DrawImageAtRotated(img, centerX, centerY, 0.6f, rad, nullptr, scale, scale);
		}

		drawCenteredText(centerY + 60.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(),
		                 Core::I18n::getInstance().get("common.loading"), 400.0f);
		return;
	}

	float sidebarX = lerp(0.0f, -SIDEBAR_WIDTH, animationProgress);
	float sidebarAlpha = lerp(1.0f, 0.0f, animationProgress);
	float channelListX = lerp(SIDEBAR_WIDTH, 0.0f, animationProgress);

	if (sidebarX > -SIDEBAR_WIDTH) {
		u32 base = ScreenManager::colorBackgroundDark();
		u8 r = base & 0xFF;
		u8 g = (base >> 8) & 0xFF;
		u8 b = (base >> 16) & 0xFF;
		C2D_DrawRectSolid(sidebarX, 0, 0.4f, SIDEBAR_WIDTH, 240, C2D_Color32(r, g, b, (u8)(255 * sidebarAlpha)));

		int visibleItems = 5;
		float itemHeight = 48.0f;
		float y = 0.0f;

		for (int i = scrollOffset; i < (int)listItems.size() && i < scrollOffset + visibleItems; ++i) {
			drawListItem(i, listItems[i], sidebarX, y);
			y += itemHeight;
		}
	}

	drawChannelList(channelListX, 0.0f, 1.0f);

	if (isMuteMenuOpen) {
		drawMuteMenu();
	}
}

void ServerListScreen::drawChannelList(float x, float y, float alpha) {
	float headerH = 26.0f;
	if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
		C2D_DrawRectSolid(x, y, 0.42f, 400.0f - x, headerH, ScreenManager::colorHeaderGlass());
		C2D_DrawRectSolid(x, y + headerH - 1.0f, 0.43f, 400.0f - x, 1.0f, ScreenManager::colorHeaderBorder());

		float textX = x + 8.0f;
		drawRichText(textX, y + 4.0f, 0.5f, 0.65f, 0.65f, ScreenManager::colorText(), listItems[selectedIndex].name);
	}

	float padding = 8.0f;
	float startX = x + padding;
	float startY = y + headerH + 4.0f;

	if (sortedChannels.empty()) {
		if (selectedIndex >= 0 && selectedIndex < (int)listItems.size() && listItems[selectedIndex].isFolder) {
			return;
		}

		bool isDmPane = selectedIndex >= 0 && selectedIndex < (int)listItems.size() && listItems[selectedIndex].isDm;
		drawText(startX, startY + 20, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(),
		         Core::I18n::getInstance().get(isDmPane ? "dm.no_messages" : "channel.no_visible"));
		return;
	}

	int itemsPerPage = channelRowsPerPage();
	float rowHeight = isDmPane() ? DM_ROW_HEIGHT : 22.0f;

	int startIdx = (state == State::SELECTING_CHANNEL) ? channelScrollOffset : 0;

	int rendered = 0;
	for (size_t i = startIdx; i < sortedChannels.size() && rendered < itemsPerPage; ++i) {
		const auto &ch = sortedChannels[i];

		bool isCategory = (ch.type == 4);
		bool isSelected = (state == State::SELECTING_CHANNEL && (int)i == selectedChannelIndex);

		bool chUnread = false;
		int chMentions = 0;
		bool chMuted = false;
		if (!isCategory) {
			auto cit = channelUnreadCache.find(ch.id);
			if (cit != channelUnreadCache.end()) {
				chUnread = cit->second.isUnread;
				chMentions = cit->second.mentionCount;
				chMuted = cit->second.muted;
			}
		}

		if (ch.type == 1 || ch.type == 3) {
			const float dmHeight = rowHeight;
			float dmY = startY + (rendered * rowHeight);
			u32 dmColor = (isSelected || chUnread) ? ScreenManager::colorText() : ScreenManager::colorTextMuted();

			if (isSelected) {
				drawRoundedRect(x + 4, dmY + 1.0f, 0.5f, 400 - x - 8, dmHeight - 2.0f, 6.0f,
				                ScreenManager::colorBackgroundLight());
			}

			const float dotR = 3.0f;
			const float dotCX = x + 8.0f;
			if (chUnread) {
				drawCircle(dotCX, dmY + dmHeight / 2.0f, 0.51f, dotR, ScreenManager::colorText());
			}

			const float avatarSize = 32.0f;
			float avatarX = dotCX + dotR + 5.0f;

			C3D_Tex *avatarTex = nullptr;
			if (ch.type == 1 && !ch.recipients.empty()) {
				const auto &r = ch.recipients[0];
				avatarTex = Discord::AvatarCache::getInstance().getAvatar(r.id, r.avatar, r.discriminator);
			} else if (ch.type == 3 && !ch.icon.empty()) {
				avatarTex = Discord::AvatarCache::getInstance().getChannelIcon(ch.id, ch.icon);
			}

			if (avatarTex) {
				Tex3DS_SubTexture sub = {(u16)avatarTex->width, (u16)avatarTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
				C2D_Image img = {avatarTex, &sub};
				C2D_DrawImageAt(img, avatarX, dmY + (dmHeight - avatarSize) / 2.0f, 0.5f, nullptr,
				                avatarSize / avatarTex->width, avatarSize / avatarTex->height);
			} else {
				C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord-icons/chat.png");
				if (tex) {
					Tex3DS_SubTexture sub = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
					C2D_Image img = {tex, &sub};
					C2D_ImageTint tint;
					C2D_PlainImageTint(&tint, dmColor, 1.0f);
					const float fallback = 24.0f;
					C2D_DrawImageAt(img, avatarX + (avatarSize - fallback) / 2.0f, dmY + (dmHeight - fallback) / 2.0f,
					                0.5f, &tint, fallback / tex->width, fallback / tex->height);
				}
			}

			float nameX = avatarX + avatarSize + 8.0f;
			float nameLimit = 400.0f - nameX - 10.0f;
			drawRichText(nameX, dmY + dmHeight / 2.0f - 8.0f, 0.5f, 0.55f, 0.55f, dmColor,
			             getTruncatedRichText(MessageUtils::getChannelDisplayName(ch), nameLimit, 0.55f, 0.55f));

			rendered++;
			continue;
		}

		u32 color;
		if (isSelected && chMuted) {
			color = ScreenManager::colorTextMuted();
		} else if (isSelected) {
			color = ScreenManager::colorText();
		} else if (chMuted) {
			color = C2D_Color32(80, 80, 88, 255);
		} else if (chUnread || chMentions > 0) {
			color = ScreenManager::colorText();
		} else {
			color = ScreenManager::colorTextMuted();
		}

		float currentY = startY + (rendered * rowHeight);
		float currentX = startX + (ch.parent_id.empty() ? 0 : 10.0f);

		if (isSelected) {
			drawRoundedRect(x + 4, currentY, 0.5f, 400 - x - 8, rowHeight, 4.0f, ScreenManager::colorBackgroundLight());
		}

		if (chUnread && !isCategory) {
			const float dotR = 2.5f;
			float dotCX = x + 5.0f;
			float dotCY = currentY + rowHeight / 2.0f;
			drawRoundedRect(dotCX - dotR, dotCY - dotR, 0.51f, dotR * 2.0f, dotR * 2.0f, dotR,
			                ScreenManager::colorText());
		}

		std::string name = ch.name;
		if (isCategory) {
			drawRichText(currentX, currentY + 4.0f, 0.5f, 0.45f, 0.45f, color, name);
		} else {
			std::string rulesId;
			if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
				const auto &item = listItems[selectedIndex];
				if (!item.isFolder) {
					const auto *guild = getGuild(item.id);
					if (guild) {
						rulesId = guild->rules_channel_id;
					}
				}
			}

			std::string iconPath;
			if (!ch.viewable) {
				iconPath = "romfs:/discord-icons/lock.png";
			} else if (!rulesId.empty() && ch.id == rulesId) {
				iconPath = "romfs:/discord-icons/bookcheck.png";
			} else if (ch.type == 2) {
				iconPath = "romfs:/discord-icons/voice.png";
			} else if (ch.type == 5) {
				iconPath = "romfs:/discord-icons/announcement.png";
			} else if (ch.type == 13) {
				iconPath = "romfs:/discord-icons/stage.png";
			} else if (ch.type == 15) {
				iconPath = "romfs:/discord-icons/forum.png";
			} else if (ch.type == 1 || ch.type == 3) {
				iconPath = "romfs:/discord-icons/chat.png";
			} else {
				iconPath = "romfs:/discord-icons/text.png";
			}

			float iconOffset = 0.0f;
			C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage(iconPath);
			if (tex) {
				Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
				C2D_Image img = {tex, &subtex};

				float iconSize = 12.0f;
				float iconY = currentY + (rowHeight - iconSize) / 2.0f;

				C2D_ImageTint tint;
				C2D_PlainImageTint(&tint, color, 1.0f);
				C2D_DrawImageAt(img, currentX, iconY, 0.5f, &tint, iconSize / tex->width, iconSize / tex->height);
				iconOffset = iconSize + 4.0f;
			}

			drawRichText(currentX + iconOffset, currentY + 3.0f, 0.5f, 0.5f, 0.5f, color, name);

			if (chMentions > 0) {
				float badgeR = 6.0f;
				float badgeX = x + (400.0f - x) - badgeR - 6.0f;
				float badgeCY = currentY + rowHeight / 2.0f;
				drawCircle(badgeX, badgeCY, 0.52f, badgeR, C2D_Color32(237, 66, 69, 255));
				std::string cntStr = chMentions > 99 ? "99+" : std::to_string(chMentions);
				float tScale = chMentions > 9 ? 0.3f : 0.35f;
				drawText(badgeX - (chMentions > 9 ? 4.5f : 2.5f), badgeCY - 4.5f, 0.53f, tScale, tScale,
				         C2D_Color32(255, 255, 255, 255), cntStr);
			}
		}
		rendered++;

		if (ch.type == 2 || ch.type == 13) {
			auto participants = Discord::DiscordClient::getInstance().getVoiceParticipants(ch.id);
			for (const auto &p : participants) {
				if (rendered >= itemsPerPage) {
					break;
				}

				float pY = startY + (rendered * rowHeight);
				float pX = currentX + 14.0f;

				float avatarSize = 14.0f;
				float avatarY = pY + (rowHeight - avatarSize) / 2.0f;

				if (Discord::VoiceClient::getInstance().isSpeaking(p.userId)) {
					drawCircle(pX + avatarSize / 2.0f, avatarY + avatarSize / 2.0f, 0.49f, avatarSize / 2.0f + 1.5f,
					           C2D_Color32(35, 165, 90, 255));
				}

				C3D_Tex *avatar = Discord::AvatarCache::getInstance().getAvatar(p.userId, p.avatar, "0");
				if (avatar) {
					Tex3DS_SubTexture sub = {(u16)avatar->width, (u16)avatar->height, 0.0f, 1.0f, 1.0f, 0.0f};
					C2D_Image img = {avatar, &sub};
					C2D_DrawImageAt(img, pX, avatarY, 0.5f, nullptr, avatarSize / avatar->width,
					                avatarSize / avatar->height);
				}

				u32 nameColor = ScreenManager::colorTextMuted();
				float nameX = pX + avatarSize + 5.0f;
				int stateIconCount = (p.mute || p.selfMute ? 1 : 0) + (p.deaf || p.selfDeaf ? 1 : 0);
				float nameLimit = 400.0f - nameX - 10.0f - stateIconCount * 14.0f;
				drawRichText(nameX, pY + 4.0f, 0.5f, 0.42f, 0.42f, nameColor,
				             getTruncatedRichText(p.name, nameLimit, 0.42f, 0.42f));

				const char *micIcon = p.mute       ? "romfs:/discord-icons/mic-denied.png"
				                      : p.selfMute ? "romfs:/discord-icons/mic-muted.png"
				                                   : nullptr;
				const char *deafIcon = p.deaf       ? "romfs:/discord-icons/headphones-denied.png"
				                       : p.selfDeaf ? "romfs:/discord-icons/headphones-muted.png"
				                                    : nullptr;

				const float stateSize = 11.0f;
				float stateX = 400.0f - stateSize - 8.0f;
				const struct {
					const char *path;
					bool byServer;
				} stateIcons[] = {{deafIcon, p.deaf}, {micIcon, p.mute}};

				for (const auto &entry : stateIcons) {
					if (!entry.path) {
						continue;
					}
					C3D_Tex *st = UI::ImageManager::getInstance().getLocalImage(entry.path);
					if (st) {
						Tex3DS_SubTexture sub = {(u16)st->width, (u16)st->height, 0.0f, 1.0f, 1.0f, 0.0f};
						C2D_Image img = {st, &sub};
						C2D_ImageTint tint;
						C2D_PlainImageTint(&tint, entry.byServer ? ScreenManager::colorError() : nameColor, 1.0f);
						C2D_DrawImageAt(img, stateX, pY + (rowHeight - stateSize) / 2.0f, 0.5f, &tint,
						                stateSize / st->width, stateSize / st->height);
					}
					stateX -= stateSize + 3.0f;
				}

				rendered++;
			}
		}
	}
}

static void drawIconUnreadIndicator(float iconX, float iconY, float iconSize, float leftDotX, float iconCenterY,
                                    int mentionCount, bool hasUnread, bool isSelected) {
	if (mentionCount > 0) {
		float badgeR = 7.0f;
		float badgeX = iconX + iconSize - badgeR + 2.0f;
		float badgeY = iconY + iconSize - badgeR + 2.0f;
		drawCircle(badgeX, badgeY, 0.52f, badgeR + 1.5f, ScreenManager::colorBackgroundDark());
		drawCircle(badgeX, badgeY, 0.53f, badgeR, C2D_Color32(237, 66, 69, 255));
		std::string countStr = mentionCount > 99 ? "99+" : std::to_string(mentionCount);
		float tScale = mentionCount > 9 ? 0.35f : 0.4f;
		drawText(badgeX - (mentionCount > 9 ? 5.0f : 3.0f), badgeY - 5.0f, 0.54f, tScale, tScale,
		         C2D_Color32(255, 255, 255, 255), countStr);
	}

	if (hasUnread && !isSelected) {
		const float dotR = 3.0f;
		drawRoundedRect(leftDotX - dotR, iconCenterY - dotR, 0.52f, dotR * 2.0f, dotR * 2.0f, dotR,
		                ScreenManager::colorText());
	}
}

void ServerListScreen::drawListItem(int index, const ListItem &item, float x, float y) {
	float width = SIDEBAR_WIDTH;

	bool isSelected = (index == selectedIndex);

	if (isSelected) {
		drawRoundedRect(x + 2, y + 10, 0.5f, 4, 28, 2.0f, ScreenManager::colorText());
	}

	float iconSize = 42.0f;
	float iconX = x + (width - iconSize) / 2.0f;
	if (item.depth > 0) {
		iconSize = 36.0f;
		iconX = x + (width - iconSize) / 2.0f;
	}
	float iconY = y + (48.0 - iconSize) / 2.0f;

	bool inExpandedFolder = (item.isFolder && item.expanded) || (item.depth > 0);
	if (inExpandedFolder) {
		bool roundTop = true;
		bool roundBottom = true;

		if (index > 0) {
			const auto &prev = listItems[index - 1];
			if ((prev.isFolder && prev.expanded) || prev.depth > 0) {
				roundTop = false;
			}
		}
		if (index < (int)listItems.size() - 1) {
			const auto &next = listItems[index + 1];
			if (next.depth > 0) {
				roundBottom = false;
			}
		}

		u32 folderBg = ScreenManager::colorBackground();
		float fX = x + 12;
		float fY = y + (roundTop ? 2 : 0);
		float fW = width - 24;
		float fH = 48 - (roundTop ? 2 : 0) - (roundBottom ? 2 : 0);

		const float capR = 12.0f;
		float topR = roundTop ? capR : 0.0f;
		float botR = roundBottom ? capR : 0.0f;

		// The body stops short of each rounded end, otherwise it fills the very
		// corners the caps are meant to round off.
		C2D_DrawRectSolid(fX, fY + topR, 0.45f, fW, fH - topR - botR, folderBg);

		if (roundTop) {
			drawCircle(fX + topR, fY + topR, 0.45f, topR, folderBg);
			drawCircle(fX + fW - topR, fY + topR, 0.45f, topR, folderBg);
			C2D_DrawRectSolid(fX + topR, fY, 0.45f, fW - topR * 2.0f, topR, folderBg);
		}
		if (roundBottom) {
			drawCircle(fX + botR, fY + fH - botR, 0.45f, botR, folderBg);
			drawCircle(fX + fW - botR, fY + fH - botR, 0.45f, botR, folderBg);
			C2D_DrawRectSolid(fX + botR, fY + fH - botR, 0.45f, fW - botR * 2.0f, botR, folderBg);
		}
	}

	if (item.isDm) {
		drawRoundedRect(iconX, iconY, 0.49f, iconSize, iconSize, iconSize / 3.0f,
		                ScreenManager::colorBackgroundLight());

		C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord.png", true);
		if (tex) {
			Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {tex, &subtex};
			float logoSize = iconSize * 0.68f;
			float logoOffset = (iconSize - logoSize) / 2.0f;
			C2D_ImageTint tint;
			C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);
			C2D_DrawImageAt(img, iconX + logoOffset, iconY + logoOffset, 0.5f, &tint, logoSize / tex->width,
			                logoSize / tex->height);
		}

		drawIconUnreadIndicator(iconX, iconY, iconSize, x + 6.0f, iconY + iconSize / 2.0f, item.mentionCount,
		                        item.hasUnread, isSelected);
	} else if (item.isFolder) {
		if (item.expanded) {
			float smallIconSize = 24.0f;
			float smallIconX = x + (width - smallIconSize) / 2.0f;
			float smallIconY = y + (48.0f - smallIconSize) / 2.0f;

			C3D_Tex *folderTex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord-icons/folder.png");
			if (folderTex) {
				Tex3DS_SubTexture subtex = {(u16)folderTex->width, (u16)folderTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
				C2D_Image img = {folderTex, &subtex};
				C2D_ImageTint tint;
				C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);
				C2D_DrawImageAt(img, smallIconX, smallIconY, 0.5f, &tint, smallIconSize / folderTex->width,
				                smallIconSize / folderTex->height);
			} else {

				C2D_DrawRectSolid(smallIconX, smallIconY, 0.5f, smallIconSize, smallIconSize,
				                  C2D_Color32(88, 101, 242, 100));
			}
		} else {
			u32 folderColor = item.color != 0 ? C2D_Color32((item.color >> 16) & 0xFF, (item.color >> 8) & 0xFF,
			                                                item.color & 0xFF, 100)
			                                  : C2D_Color32(88, 101, 242, 100);
			drawRoundedRect(iconX, iconY, 0.5f, iconSize, iconSize, iconSize / 3.0f, folderColor);

			const float miniInset = 4.0f;
			const float miniGap = 2.0f;
			const float miniSize = 16.0f;
			for (size_t i = 0; i < std::min((size_t)4, item.folderGuildIds.size()); ++i) {
				const std::string &guildId = item.folderGuildIds[i];
				const Discord::Guild *g = getGuild(guildId);
				if (!g) {
					continue;
				}

				size_t miniRow = i / 2;
				size_t miniCol = i % 2;
				float mX = iconX + miniInset + (float)miniCol * (miniSize + miniGap);
				float mY = iconY + miniInset + (float)miniRow * (miniSize + miniGap);

				C3D_Tex *tex = nullptr;
				if (!g->icon.empty()) {
					tex = Discord::AvatarCache::getInstance().getGuildIcon(g->id, g->icon);
				}

				if (tex) {
					Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
					C2D_Image img = {tex, &subtex};
					C2D_DrawImageAt(img, mX, mY, 0.51f, nullptr, miniSize / tex->width, miniSize / tex->height);
				} else {
					drawRoundedRect(mX, mY, 0.51f, miniSize, miniSize, miniSize / 3.0f,
					                ScreenManager::colorBackgroundLight());
					std::string miniInit = Utils::Utf8::getFirstChar(g->name.empty() ? "?" : g->name);
					drawText(mX + miniSize / 2 - 3, mY + miniSize / 2 - 4, 0.52f, 0.3f, 0.3f,
					         ScreenManager::colorText(), miniInit);
				}
			}
		}

		if (!item.expanded) {
			drawIconUnreadIndicator(iconX, iconY, iconSize, x + 6.0f, iconY + iconSize / 2.0f, item.mentionCount,
			                        item.hasUnread, isSelected);
		}
	} else {
		C3D_Tex *tex = nullptr;

		if (!item.icon.empty()) {
			tex = Discord::AvatarCache::getInstance().getGuildIcon(item.id, item.icon);
		}

		if (tex) {
			Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {tex, &subtex};
			float sX = iconSize / tex->width;
			float sY = iconSize / tex->height;
			C2D_DrawImageAt(img, iconX, iconY, 0.5f, nullptr, sX, sY);
		} else {
			drawRoundedRect(iconX, iconY, 0.5f, iconSize, iconSize, iconSize / 3.0f,
			                ScreenManager::colorBackgroundLight());

			std::string init = Utils::Utf8::getFirstChar(item.name.empty() ? "?" : item.name);
			drawText(iconX + iconSize / 2 - 5, iconY + iconSize / 2 - 6, 0.5f, 0.5f, 0.5f, ScreenManager::colorText(),
			         init);
		}

		drawIconUnreadIndicator(iconX, iconY, iconSize, x + 6.0f, iconY + iconSize / 2.0f, item.mentionCount,
		                        item.hasUnread, isSelected);
	}
}

bool ServerListScreen::isDmPane() const {
	return selectedIndex >= 0 && selectedIndex < (int)listItems.size() && listItems[selectedIndex].isDm;
}

int ServerListScreen::channelRowsPerPage() const { return isDmPane() ? DM_ROWS_PER_PAGE : CHANNEL_ROWS_PER_PAGE; }

int ServerListScreen::channelRowSpan(int index) const {
	if (index < 0 || index >= (int)sortedChannels.size()) {
		return 1;
	}
	const auto &ch = sortedChannels[index];
	if (ch.type != 2 && ch.type != 13) {
		return 1;
	}
	return 1 + (int)Discord::DiscordClient::getInstance().getVoiceParticipants(ch.id).size();
}

void ServerListScreen::ensureChannelVisible() {
	if (selectedChannelIndex < channelScrollOffset) {
		channelScrollOffset = selectedChannelIndex;
		return;
	}

	int rows = 0;
	for (int i = channelScrollOffset; i <= selectedChannelIndex; i++) {
		rows += channelRowSpan(i);
	}

	while (rows > channelRowsPerPage() && channelScrollOffset < selectedChannelIndex) {
		rows -= channelRowSpan(channelScrollOffset);
		channelScrollOffset++;
	}
}

void ServerListScreen::drawVoiceStatus() {
	Discord::VoiceState vs = Discord::VoiceClient::getInstance().getState();
	if (vs == Discord::VoiceState::DISCONNECTED) {
		return;
	}

	std::string label = "Voice: ";
	u32 color = ScreenManager::colorTextMuted();
	switch (vs) {
	case Discord::VoiceState::AWAITING_SERVER:
		label += "waiting for server";
		break;
	case Discord::VoiceState::CONNECTING:
		label += "connecting";
		break;
	case Discord::VoiceState::IDENTIFYING:
		label += "identifying";
		break;
	case Discord::VoiceState::READY:
		label += "ready";
		break;
	case Discord::VoiceState::SELECTING_PROTOCOL:
		label += "negotiating";
		break;
	case Discord::VoiceState::ESTABLISHED:
		label += "connected";
		color = ScreenManager::colorAccent();
		break;
	case Discord::VoiceState::FAILED:
		label += "failed";
		color = ScreenManager::colorError();
		break;
	default:
		break;
	}

	drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 38.0f, 0.5f, 0.4f, 0.4f, color, label);
}

void ServerListScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_SceneBegin(target);
	C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> lock(client.getMutex());

	bool infoDrawn = false;

	if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
		const auto &item = listItems[selectedIndex];

		if (!item.isFolder) {
			const Discord::Guild *guild = getGuild(item.id);
			if (guild) {
				float headerX = 35.0f;
				C3D_Tex *tex = nullptr;
				if (!guild->icon.empty()) {
					tex = Discord::AvatarCache::getInstance().getGuildIcon(guild->id, guild->icon);
				}

				if (tex) {
					float iconSize = 18.0f;
					Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
					C2D_Image img = {tex, &subtex};
					C2D_DrawImageAt(img, headerX, 8.0f, 0.5f, nullptr, iconSize / tex->width, iconSize / tex->height);
					headerX += iconSize + 6.0f;
				}

				drawRichText(headerX, 8.5f, 0.5f, 0.55f, 0.55f, ScreenManager::colorAccent(),
				             getTruncatedRichText(guild->name, 305.0f - headerX, 0.55f, 0.55f));

				C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());

				float statsY = 40.0f;

				drawText(10.0f, statsY, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
				         TR("server.member_count") + ":");
				drawText(10.0f, statsY + 12.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorText(),
				         std::to_string(guild->approximateMemberCount));

				drawText(100.0f, statsY, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
				         TR("server.online_count") + ":");
				drawText(100.0f, statsY + 12.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorSuccess(),
				         std::to_string(guild->approximatePresenceCount));

				float overviewY = statsY + 35.0f;
				drawText(10.0f, overviewY, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(),
				         TR("server.description"));
				overviewY += 15.0f;

				std::string desc = guild->description;
				if (desc.empty()) {
					desc = TR("common.no_topic");
				}

				auto lines = MessageUtils::wrapText(desc, 300.0f, 0.4f);
				int lineCount = 0;
				for (const auto &line : lines) {
					if (lineCount >= 10) {
						break;
					}
					drawRichText(10.0f, overviewY, 0.5f, 0.4f, 0.4f, ScreenManager::colorText(), line);
					overviewY += 13.0f;
					lineCount++;
				}
				infoDrawn = true;
			}
		} else {
			drawRichText(35.0f, 8.5f, 0.5f, 0.55f, 0.55f, ScreenManager::colorAccent(),
			             getTruncatedRichText(item.name, 305.0f - 35.0f, 0.55f, 0.55f));
			C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());

			float infoY = 40.0f;
			std::string countStr = Core::I18n::format(TR("server.count"), std::to_string(item.folderGuildIds.size()));
			drawText(10.0f, infoY, 0.5f, 0.45f, 0.45f, ScreenManager::colorText(), countStr);

			infoY += 18.0f;
			drawText(10.0f, infoY, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(), TR("server.list") + ":");
			infoY += 14.0f;

			int displayCount = 0;
			for (const auto &guildId : item.folderGuildIds) {
				if (displayCount >= 9) {
					break;
				}
				const Discord::Guild *g = getGuild(guildId);
				if (g) {
					std::string guildName = getTruncatedRichText(g->name, 300.0f, 0.4f, 0.4f);
					drawRichText(15.0f, infoY, 0.5f, 0.4f, 0.4f, ScreenManager::colorText(), guildName);
					infoY += 13.0f;
					displayCount++;
				}
			}

			if (item.folderGuildIds.size() > 9) {
				int remaining = item.folderGuildIds.size() - 9;
				drawRichText(15.0f, infoY, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
				             "+" + std::to_string(remaining) + " more");
			}

			infoDrawn = true;
		}
	}

	if (!infoDrawn) {
		std::string title = (state == State::SELECTING_SERVER) ? TR("server.select") : TR("channel.select");
		drawText(35.0f, 8.5f, 0.5f, 0.55f, 0.55f, ScreenManager::colorText(), title);
		C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());
	}

	if (state == State::SELECTING_SERVER) {
		drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 25.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		         "\uE079\uE07A: " + TR("common.navigate") + "  \uE000: " + TR("common.enter") +
		             "  START: " + TR("common.exit"));
	} else {
		drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 25.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		         "\uE079\uE07A: " + TR("common.navigate") + "  \uE001: " + TR("common.back") +
		             "  \uE000: " + TR("common.enter"));
	}

	drawVoiceStatus();
	VoiceControls::draw(VOICE_BTN_X, VOICE_BTN_Y);
}

} // namespace UI
