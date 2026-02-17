#include "ui/server_list_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "log.h"
#include "ui/image_manager.h"
#include "utils/message_utils.h"
#include <3ds.h>
#include <algorithm>
#include <cstdio>

namespace UI {

ServerListScreen::ServerListScreen()
    : repeatTimer(0), lastKey(0), animationProgress(0.0f), loadingAngle(0.0f),
      animTimer(0.0f) {
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
    if (!listItems[selectedIndex].isFolder) {
      Discord::DiscordClient::getInstance().fetchGuildDetails(
          listItems[selectedIndex].id);
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
    if (g.id == id)
      return &g;
  }
  return nullptr;
}

ServerListScreen::ListItem
ServerListScreen::createGuildItem(const Discord::Guild *g, int depth) {
  ListItem item;
  item.isFolder = false;
  item.id = g->id;
  item.name = g->name;
  item.icon = g->icon;
  item.depth = depth;
  return item;
}

ServerListScreen::ListItem
ServerListScreen::createFolderItem(const Discord::GuildFolder &f) {
  ListItem item;
  item.isFolder = true;
  item.id = f.id;
  item.name =
      f.name.empty() ? Core::I18n::getInstance().get("common.folder") : f.name;
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
  std::vector<std::string> visitedGuilds;

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
            visitedGuilds.push_back(g->id);
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
              visitedGuilds.push_back(g->id);
            }
          }
        } else {
          for (const auto &gid : f.guildIds) {
            visitedGuilds.push_back(gid);
          }
        }
      }
    }

    const auto &allGuilds = client.getGuilds();
    std::vector<ListItem> orphans;
    for (const auto &g : allGuilds) {
      bool alreadyAdded = false;
      for (const auto &vid : visitedGuilds) {
        if (vid == g.id) {
          alreadyAdded = true;
          break;
        }
      }

      if (!alreadyAdded) {
        orphans.push_back(createGuildItem(&g, 0));
      }
    }

    if (!orphans.empty()) {
      listItems.insert(listItems.begin(), orphans.begin(), orphans.end());
    }
  }
  Logger::log("ServerListScreen::rebuildList() end, items: %d",
              (int)listItems.size());
}

void ServerListScreen::refreshChannels() {
  Logger::log("ServerListScreen::refreshChannels() start");
  sortedChannels.clear();
  if (listItems.empty() || selectedIndex < 0 ||
      selectedIndex >= (int)listItems.size()) {
    Logger::log("ServerListScreen::refreshChannels() quick return");
    return;
  }

  const auto &item = listItems[selectedIndex];
  if (item.isFolder)
    return;

  const auto *guild = getGuild(item.id);
  if (!guild)
    return;

  std::vector<Discord::Channel> viewableChannels;
  std::vector<Discord::Channel> categories;

  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  const auto &currentUser = client.getCurrentUser();
  bool isOwner = (guild->ownerId == currentUser.id);

  for (const auto &ch : guild->channels) {
    if (ch.type == 4) {
      categories.push_back(ch);
    } else if (ch.viewable || isOwner) {
      viewableChannels.push_back(ch);
    }
  }

  auto sortByPosAndType = [](const Discord::Channel &a,
                             const Discord::Channel &b) {
    bool aIsVoice = (a.type == 2 || a.type == 13);
    bool bIsVoice = (b.type == 2 || b.type == 13);

    if (aIsVoice != bIsVoice) {
      return !aIsVoice;
    }

    if (a.position != b.position) {
      return a.position < b.position;
    }

    if (a.id.length() != b.id.length())
      return a.id.length() < b.id.length();
    return a.id < b.id;
  };

  std::sort(categories.begin(), categories.end(),
            [](const Discord::Channel &a, const Discord::Channel &b) {
              if (a.position != b.position)
                return a.position < b.position;
              if (a.id.length() != b.id.length())
                return a.id.length() < b.id.length();
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
}

void ServerListScreen::update() {
  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  auto &sm = ScreenManager::getInstance();
  std::lock_guard<std::recursive_mutex> lock(client.getMutex());
  client.update();

  if (listItems.empty()) {
    if (!client.getGuilds().empty()) {
      rebuildList();
      refreshChannels();
    } else {
      animTimer += 1.0f / 60.0f;
      if (animTimer >= 1.5f)
        animTimer = 0.0f;

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

  if (listItems.empty())
    return;

  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();

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
    }
    return;
  }

  u32 moveDir = 0;
  if (kDown & KEY_DOWN)
    moveDir = KEY_DOWN;
  else if (kDown & KEY_UP)
    moveDir = KEY_UP;
  else if (kHeld & KEY_DOWN && (--repeatTimer <= 0)) {
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
  if (!(kHeld & (KEY_DOWN | KEY_UP)))
    lastKey = 0;

  if (state == State::SELECTING_SERVER) {
    bool selectionChanged = false;
    if (moveDir & KEY_DOWN) {
      if (selectedIndex < (int)listItems.size() - 1) {
        selectedIndex++;
        if (selectedIndex >= scrollOffset + 5)
          scrollOffset = selectedIndex - 4;
        selectionChanged = true;
      }
    } else if (moveDir & KEY_UP) {
      if (selectedIndex > 0) {
        selectedIndex--;
        if (selectedIndex < scrollOffset)
          scrollOffset = selectedIndex;
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
        if (!listItems[selectedIndex].isFolder) {
          Discord::DiscordClient::getInstance().fetchGuildDetails(
              listItems[selectedIndex].id);
        }
      }
    }

    if (kDown & KEY_A) {
      if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
        const auto &item = listItems[selectedIndex];
        if (item.isFolder) {
          bool isExpanded =
              ScreenManager::getInstance().isFolderExpanded(item.id);
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
          while (nextIndex < (int)sortedChannels.size() &&
                 sortedChannels[nextIndex].type == 4) {
            nextIndex++;
          }
          if (nextIndex < (int)sortedChannels.size()) {
            selectedChannelIndex = nextIndex;
            if (selectedChannelIndex >= channelScrollOffset + 9)
              channelScrollOffset = selectedChannelIndex - 8;
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
            if (selectedChannelIndex < channelScrollOffset)
              channelScrollOffset = selectedChannelIndex;
          }
        }
      }

      if (moveDir & (KEY_UP | KEY_DOWN)) {
        sm.setLastChannelIndex(sm.getSelectedGuildId(), selectedChannelIndex);
        sm.setLastChannelScroll(sm.getSelectedGuildId(), channelScrollOffset);
      }
    }

    if (kDown & KEY_B) {
      state = State::TRANSITION_TO_SERVER;
      sm.setLastChannelIndex(sm.getSelectedGuildId(), -1);
      sm.setLastChannelScroll(sm.getSelectedGuildId(), 0);
    } else if (kDown & KEY_A) {
      if (selectedChannelIndex >= 0 &&
          selectedChannelIndex < (int)sortedChannels.size()) {
        const auto &ch = sortedChannels[selectedChannelIndex];
        if (ch.type == 0 || ch.type == 5 || ch.type == 10 || ch.type == 11 ||
            ch.type == 12 || ch.type == 1 || ch.type == 3) {
          Discord::DiscordClient::getInstance().setSelectedChannelId(ch.id);
          ScreenManager::getInstance().setScreen(ScreenType::MESSAGES);
        } else if (ch.type == 15) {
          Discord::DiscordClient::getInstance().setSelectedChannelId(ch.id);
          ScreenManager::getInstance().setScreen(ScreenType::FORUM_CHANNEL);
        }
      }
    }
  }
}

void ServerListScreen::renderTop(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackground());

  if (listItems.empty()) {
    float centerX = 200.0f;
    float centerY = 120.0f;

    C3D_Tex *discordTex = UI::ImageManager::getInstance().getLocalImage(
        "romfs:/discord.png", true);
    if (discordTex) {
      UI::ImageManager::ImageInfo info =
          UI::ImageManager::getInstance().getImageInfo("romfs:/discord.png");
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
      C2D_DrawImageAtRotated(img, centerX, centerY, 0.6f, rad, nullptr, scale,
                             scale);
    }

    drawCenteredText(centerY + 60.0f, 0.5f, 0.5f, 0.5f,
                     ScreenManager::colorTextMuted(),
                     Core::I18n::getInstance().get("server.loading"), 400.0f);
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
    C2D_DrawRectSolid(sidebarX, 0, 0.4f, SIDEBAR_WIDTH, 240,
                      C2D_Color32(r, g, b, (u8)(255 * sidebarAlpha)));

    int visibleItems = 5;
    float itemHeight = 48.0f;
    float y = 0.0f;

    for (int i = scrollOffset;
         i < (int)listItems.size() && i < scrollOffset + visibleItems; ++i) {
      drawListItem(i, listItems[i], sidebarX, y);
      y += itemHeight;
    }
  }

  drawChannelList(channelListX, 0.0f, 1.0f);
}

void ServerListScreen::drawChannelList(float x, float y, float alpha) {
  float headerH = 26.0f;
  if (selectedIndex >= 0 && selectedIndex < (int)listItems.size()) {
    C2D_DrawRectSolid(x, y, 0.42f, 400.0f - x, headerH,
                      ScreenManager::colorHeaderGlass());
    C2D_DrawRectSolid(x, y + headerH - 1.0f, 0.43f, 400.0f - x, 1.0f,
                      ScreenManager::colorHeaderBorder());

    float textX = x + 8.0f;
    drawRichText(textX, y + 4.0f, 0.5f, 0.65f, 0.65f,
                 ScreenManager::colorText(), listItems[selectedIndex].name);
  }

  float padding = 8.0f;
  float startX = x + padding;
  float startY = y + headerH + 4.0f;

  if (sortedChannels.empty()) {
    if (selectedIndex >= 0 && selectedIndex < (int)listItems.size() &&
        listItems[selectedIndex].isFolder) {
      return;
    }

    drawText(startX, startY + 20, 0.5f, 0.5f, 0.5f,
             ScreenManager::colorTextMuted(),
             Core::I18n::getInstance().get("channel.no_visible"));
    return;
  }

  int itemsPerPage = 9;
  float rowHeight = 22.0f;

  int startIdx = (state == State::SELECTING_CHANNEL) ? channelScrollOffset : 0;

  int rendered = 0;
  for (size_t i = startIdx;
       i < sortedChannels.size() && rendered < itemsPerPage; ++i) {
    const auto &ch = sortedChannels[i];

    bool isCategory = (ch.type == 4);
    bool isSelected =
        (state == State::SELECTING_CHANNEL && (int)i == selectedChannelIndex);

    u32 color = ScreenManager::colorTextMuted();
    if (isSelected)
      color = ScreenManager::colorText();

    float currentY = startY + (rendered * rowHeight);
    float currentX = startX + (ch.parent_id.empty() ? 0 : 10.0f);

    if (isSelected) {
      drawRoundedRect(x + 4, currentY, 0.5f, 400 - x - 8, rowHeight, 4.0f,
                      ScreenManager::colorBackgroundLight());
    }

    std::string name = ch.name;
    if (isCategory) {
      drawRichText(currentX, currentY + 4.0f, 0.5f, 0.45f, 0.45f, color, name);
    } else {
      std::string iconPath = "romfs:/discord-icons/text.png";
      if (ch.type == 2)
        iconPath = "romfs:/discord-icons/voice.png";
      else if (ch.type == 5)
        iconPath = "romfs:/discord-icons/announcement.png";
      else if (ch.type == 13)
        iconPath = "romfs:/discord-icons/stage.png";
      else if (ch.type == 15)
        iconPath = "romfs:/discord-icons/forum.png";
      else if (ch.type == 1 || ch.type == 3)
        iconPath = "romfs:/discord-icons/chat.png";

      float iconOffset = 0.0f;
      C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage(iconPath);
      if (tex) {
        Tex3DS_SubTexture subtex = {
            (u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
        C2D_Image img = {tex, &subtex};

        float iconSize = 12.0f;
        float iconY = currentY + (rowHeight - iconSize) / 2.0f;

        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, color, 1.0f);
        C2D_DrawImageAt(img, currentX, iconY, 0.5f, &tint,
                        iconSize / tex->width, iconSize / tex->height);
        iconOffset = iconSize + 4.0f;
      }

      drawRichText(currentX + iconOffset, currentY + 3.0f, 0.5f, 0.5f, 0.5f,
                   color, name);
    }
    rendered++;
  }
}

void ServerListScreen::drawListItem(int index, const ListItem &item, float x,
                                    float y) {
  float width = SIDEBAR_WIDTH;

  bool isSelected = (index == selectedIndex);

  if (isSelected) {
    drawRoundedRect(x + 2, y + 10, 0.5f, 4, 28, 2.0f,
                    ScreenManager::colorText());
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
      if ((prev.isFolder && prev.expanded) || prev.depth > 0)
        roundTop = false;
    }
    if (index < (int)listItems.size() - 1) {
      const auto &next = listItems[index + 1];
      if (next.depth > 0)
        roundBottom = false;
    }

    u32 folderBg = ScreenManager::colorBackground();
    float fX = x + 12;
    float fY = y + (roundTop ? 2 : 0);
    float fW = width - 24;
    float fH = 48 - (roundTop ? 2 : 0) - (roundBottom ? 2 : 0);

    C2D_DrawRectSolid(fX, fY, 0.45f, fW, fH, folderBg);
    if (roundTop) {
      drawCircle(fX + 12.0f, fY + 12.0f, 0.455f, 12.0f, folderBg);
      drawCircle(fX + fW - 12.0f, fY + 12.0f, 0.455f, 12.0f, folderBg);
      C2D_DrawRectSolid(fX + 12.0f, fY, 0.455f, fW - 24.0f, 12.0f, folderBg);
    }
    if (roundBottom) {
      drawCircle(fX + 12.0f, fY + fH - 12.0f, 0.455f, 12.0f, folderBg);
      drawCircle(fX + fW - 12.0f, fY + fH - 12.0f, 0.455f, 12.0f, folderBg);
      C2D_DrawRectSolid(fX + 12.0f, fY + fH - 12.0f, 0.455f, fW - 24.0f, 12.0f,
                        folderBg);
    }
  }

  if (item.isFolder) {
    if (item.expanded) {
      float smallIconSize = 24.0f;
      float smallIconX = x + (width - smallIconSize) / 2.0f;
      float smallIconY = y + (48.0f - smallIconSize) / 2.0f;

      C3D_Tex *folderTex = UI::ImageManager::getInstance().getLocalImage(
          "romfs:/discord-icons/folder.png");
      if (folderTex) {
        Tex3DS_SubTexture subtex = {(u16)folderTex->width,
                                    (u16)folderTex->height,
                                    0.0f,
                                    1.0f,
                                    1.0f,
                                    0.0f};
        C2D_Image img = {folderTex, &subtex};
        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);
        C2D_DrawImageAt(img, smallIconX, smallIconY, 0.5f, &tint,
                        smallIconSize / folderTex->width,
                        smallIconSize / folderTex->height);
      } else {

        C2D_DrawRectSolid(smallIconX, smallIconY, 0.5f, smallIconSize,
                          smallIconSize, C2D_Color32(88, 101, 242, 100));
      }
    } else {
      u32 folderColor = item.color != 0 ? C2D_Color32((item.color >> 16) & 0xFF,
                                                      (item.color >> 8) & 0xFF,
                                                      item.color & 0xFF, 100)
                                        : C2D_Color32(88, 101, 242, 100);
      C2D_DrawRectSolid(iconX, iconY, 0.5f, iconSize, iconSize, folderColor);

      float miniSize = (iconSize - 6.0f) / 2.0f;
      for (size_t i = 0; i < std::min((size_t)4, item.folderGuildIds.size());
           ++i) {
        const std::string &guildId = item.folderGuildIds[i];
        const Discord::Guild *g = getGuild(guildId);
        if (!g)
          continue;

        float mX = iconX + 2.0f + (i % 2) * (miniSize + 2.0f);
        float mY = iconY + 2.0f + (i / 2) * (miniSize + 2.0f);

        C3D_Tex *tex = nullptr;
        if (!g->icon.empty()) {
          std::string iconKey = g->id + "_" + g->icon;
          auto it = iconCache.find(iconKey);
          if (it != iconCache.end()) {
            tex = it->second;
          } else {
            tex = Discord::AvatarCache::getInstance().getGuildIcon(g->id,
                                                                   g->icon);
            if (tex) {
              iconCache[iconKey] = tex;
            } else {
              Discord::AvatarCache::getInstance().prefetchGuildIcon(g->id,
                                                                    g->icon);
            }
          }
        }

        if (tex) {
          Tex3DS_SubTexture subtex = {
              (u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
          C2D_Image img = {tex, &subtex};
          C2D_DrawImageAt(img, mX, mY, 0.51f, nullptr, miniSize / tex->width,
                          miniSize / tex->height);
        } else {
          C2D_DrawRectSolid(mX, mY, 0.51f, miniSize, miniSize,
                            ScreenManager::colorBackgroundLight());
        }
      }
    }
  } else {
    std::string iconKey = item.id + "_" + item.icon;
    C3D_Tex *tex = nullptr;

    if (!item.icon.empty()) {
      auto it = iconCache.find(iconKey);
      if (it != iconCache.end()) {
        tex = it->second;
      } else {
        tex = Discord::AvatarCache::getInstance().getGuildIcon(item.id,
                                                               item.icon);
        if (tex) {
          iconCache[iconKey] = tex;
        } else {
          Discord::AvatarCache::getInstance().prefetchGuildIcon(item.id,
                                                                item.icon);
        }
      }
    }

    if (tex) {
      Tex3DS_SubTexture subtex = {
          (u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
      C2D_Image img = {tex, &subtex};
      float sX = iconSize / tex->width;
      float sY = iconSize / tex->height;
      C2D_DrawImageAt(img, iconX, iconY, 0.5f, nullptr, sX, sY);
    } else {
      C2D_DrawRectSolid(iconX, iconY, 0.5f, iconSize, iconSize,
                        ScreenManager::colorBackgroundLight());

      std::string init = item.name.empty() ? "?" : item.name.substr(0, 1);
      drawText(iconX + iconSize / 2 - 5, iconY + iconSize / 2 - 6, 0.5f, 0.5f,
               0.5f, ScreenManager::colorText(), init);
    }
  }
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
        std::string iconKey = guild->id + "_" + guild->icon;
        C3D_Tex *tex = nullptr;
        auto it = iconCache.find(iconKey);
        if (it != iconCache.end())
          tex = it->second;

        if (tex) {
          float iconSize = 18.0f;
          Tex3DS_SubTexture subtex = {
              (u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
          C2D_Image img = {tex, &subtex};
          C2D_DrawImageAt(img, headerX, 8.0f, 0.5f, nullptr,
                          iconSize / tex->width, iconSize / tex->height);
          headerX += iconSize + 6.0f;
        }

        drawRichText(
            headerX, 8.5f, 0.5f, 0.55f, 0.55f, ScreenManager::colorPrimary(),
            getTruncatedRichText(guild->name, 305.0f - headerX, 0.55f, 0.55f));

        C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1,
                          ScreenManager::colorSeparator());

        float statsY = 40.0f;

        drawText(10.0f, statsY, 0.5f, 0.45f, 0.45f,
                 ScreenManager::colorTextMuted(),
                 TR("server.member_count") + ":");
        drawText(10.0f, statsY + 12.0f, 0.5f, 0.5f, 0.5f,
                 ScreenManager::colorText(),
                 std::to_string(guild->approximateMemberCount));

        drawText(100.0f, statsY, 0.5f, 0.45f, 0.45f,
                 ScreenManager::colorTextMuted(),
                 TR("server.online_count") + ":");
        drawText(100.0f, statsY + 12.0f, 0.5f, 0.5f, 0.5f,
                 ScreenManager::colorSuccess(),
                 std::to_string(guild->approximatePresenceCount));

        float overviewY = statsY + 35.0f;
        drawText(10.0f, overviewY, 0.5f, 0.45f, 0.45f,
                 ScreenManager::colorSelection(), TR("server.description"));
        overviewY += 14.0f;

        std::string desc = guild->description;
        if (desc.empty())
          desc = TR("message.no_topic");

        auto lines = MessageUtils::wrapText(desc, 300.0f, 0.4f);
        int lineCount = 0;
        for (const auto &line : lines) {
          if (lineCount >= 10)
            break;
          drawRichText(10.0f, overviewY, 0.5f, 0.4f, 0.4f,
                       ScreenManager::colorText(), line);
          overviewY += 11.0f;
          lineCount++;
        }
        infoDrawn = true;
      }
    } else {
      drawText(35.0f, 10.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorText(),
               item.name);
      C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1,
                        ScreenManager::colorSeparator());
      infoDrawn = true;
    }
  }

  if (!infoDrawn) {
    std::string title = (state == State::SELECTING_SERVER)
                            ? TR("server.select")
                            : TR("channel.select");
    drawText(45.0f, 10.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorText(), title);
  }

  if (state == State::SELECTING_SERVER) {
    drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 25.0f, 0.5f, 0.4f, 0.4f,
             ScreenManager::colorTextMuted(),
             "\uE079\uE07A: " + TR("common.navigate") + "  \uE000: " +
                 TR("common.enter") + "  START: " + TR("common.exit"));
  } else {
    drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 25.0f, 0.5f, 0.4f, 0.4f,
             ScreenManager::colorTextMuted(),
             "\uE079\uE07A: " + TR("common.navigate") + "  \uE001: " +
                 TR("common.back") + "  \uE000: " + TR("common.enter"));
  }
}

} // namespace UI
