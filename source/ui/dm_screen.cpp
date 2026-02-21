#include "ui/dm_screen.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "log.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include <3ds.h>
#include <algorithm>

namespace UI {

const float DmScreen::ITEM_HEIGHT = 48.0f;
const int DmScreen::VISIBLE_ITEMS = 4;

DmScreen::DmScreen()
    : selectedIndex(0), scrollOffset(0), repeatTimer(0), lastKey(0) {
  Logger::log("DmScreen initialized");
  refreshDms();
}

void DmScreen::refreshDms() {
  dms = Discord::DiscordClient::getInstance().getPrivateChannels();

  std::sort(dms.begin(), dms.end(),
            [](const Discord::Channel &a, const Discord::Channel &b) {
              if (a.last_message_id.length() != b.last_message_id.length())
                return a.last_message_id.length() > b.last_message_id.length();
              return a.last_message_id > b.last_message_id;
            });
}

void DmScreen::update() {
  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();

  if (dms.empty()) {
    refreshDms();
    return;
  }

  for (int i = scrollOffset;
       i < (int)dms.size() && i < scrollOffset + VISIBLE_ITEMS; ++i) {
    const auto &dm = dms[i];
    if (dm.type == 1 && !dm.recipients.empty()) {
      const auto &r = dm.recipients[0];
      Discord::AvatarCache::getInstance().prefetchAvatar(r.id, r.avatar,
                                                         r.discriminator);
    } else if (dm.type == 3 && !dm.icon.empty()) {
      Discord::AvatarCache::getInstance().prefetchGuildIcon(dm.id, dm.icon);
    }
  }

  int moveDir = 0;
  if (kDown & KEY_DOWN)
    moveDir = 1;
  else if (kDown & KEY_UP)
    moveDir = -1;
  else if (kHeld & KEY_DOWN && (--repeatTimer <= 0)) {
    moveDir = 1;
    repeatTimer = REPEAT_DELAY_CONTINUOUS;
  } else if (kHeld & KEY_UP && (--repeatTimer <= 0)) {
    moveDir = -1;
    repeatTimer = REPEAT_DELAY_CONTINUOUS;
  }

  if (kDown & (KEY_DOWN | KEY_UP)) {
    repeatTimer = REPEAT_DELAY_INITIAL;
    lastKey = (kDown & KEY_DOWN) ? KEY_DOWN : KEY_UP;
  }

  if (!(kHeld & (KEY_DOWN | KEY_UP)))
    lastKey = 0;

  if (moveDir != 0) {
    selectedIndex += moveDir;
    if (selectedIndex < 0)
      selectedIndex = 0;
    if (selectedIndex >= (int)dms.size())
      selectedIndex = (int)dms.size() - 1;

    if (selectedIndex < scrollOffset)
      scrollOffset = selectedIndex;
    if (selectedIndex >= scrollOffset + VISIBLE_ITEMS)
      scrollOffset = selectedIndex - (VISIBLE_ITEMS - 1);
  }

  if (kDown & KEY_A) {
    if (selectedIndex >= 0 && selectedIndex < (int)dms.size()) {
      const auto &dm = dms[selectedIndex];
      Discord::DiscordClient::getInstance().setSelectedChannelId(dm.id);
      ScreenManager::getInstance().pushScreen(ScreenType::MESSAGES);
    }
  }

  if (kDown & KEY_B) {
    ScreenManager::getInstance().returnToPreviousScreen();
  }
}

void DmScreen::renderTop(C3D_RenderTarget *target) {
  C2D_TargetClear(target, ScreenManager::colorBackground());
  C2D_SceneBegin(target);

  float headerH = 26.0f;
  C2D_DrawRectSolid(0, 0, 0.9f, 400.0f, headerH,
                    ScreenManager::colorHeaderGlass());
  C2D_DrawRectSolid(0, headerH - 1.0f, 0.91f, 400.0f, 1.0f,
                    ScreenManager::colorHeaderBorder());

  drawCenteredRichText(4.0f, 0.95f, 0.52f, 0.52f, ScreenManager::colorText(),
                       TR("menu.direct_messages"), 400.0f);

  if (dms.empty()) {
    drawCenteredText(100.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(),
                     TR("dm.no_messages"), 400.0f);
    return;
  }

  float y = headerH + 11.0f;
  for (int i = scrollOffset;
       i < (int)dms.size() && i < scrollOffset + VISIBLE_ITEMS; ++i) {
    drawDmItem(i, dms[i], y);
    y += ITEM_HEIGHT;
  }
}

void DmScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

  drawText(35.0f, 10.0f, 0.6f, 0.5f, 0.5f, ScreenManager::colorText(),
           TR("menu.direct_messages"));

  C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());

  if (selectedIndex >= 0 && selectedIndex < (int)dms.size()) {
    const auto &dm = dms[selectedIndex];
    std::string dispNameBottom = dm.name;
    if (dispNameBottom.empty() && dm.type == 1 && !dm.recipients.empty()) {
      dispNameBottom = dm.recipients[0].global_name;
      if (dispNameBottom.empty()) {
        dispNameBottom = dm.recipients[0].username;
      }
    }
    float maxBottomW = 310.0f - 10.0f;
    if (measureRichText(dispNameBottom, 0.6f, 0.6f) > maxBottomW) {
      while (!dispNameBottom.empty() &&
             measureRichText(dispNameBottom + "...", 0.6f, 0.6f) > maxBottomW) {
        if ((dispNameBottom.back() & 0xc0) == 0x80) {
          while (!dispNameBottom.empty() &&
                 (dispNameBottom.back() & 0xc0) == 0x80)
            dispNameBottom.pop_back();
        }
        if (!dispNameBottom.empty())
          dispNameBottom.pop_back();
      }
      dispNameBottom += "...";
    }

    drawRichText(10.0f, 40.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorPrimary(),
                 dispNameBottom);
  }

  drawText(
      10.0f, 240.0f - 25.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
      "\uE079\uE07A: " + TR("common.navigate") +
          "  \uE000: " + TR("common.open") + "  \uE001: " + TR("common.back"));
}

void DmScreen::drawDmItem(int index, const Discord::Channel &dm, float y) {
  bool isSelected = (index == selectedIndex);
  u32 bgColor = isSelected ? ScreenManager::colorBackgroundLight()
                           : ScreenManager::colorBackgroundDark();

  drawRoundedRect(10.0f, y + 2.0f, 0.4f, 380.0f, ITEM_HEIGHT - 4.0f, 8.0f,
                  bgColor);

  if (isSelected) {
    drawRoundedRect(10.0f, y + 10.0f, 0.45f, 4.0f, ITEM_HEIGHT - 20.0f, 2.0f,
                    ScreenManager::colorSelection());
  }

  u32 textColor =
      isSelected ? ScreenManager::colorText() : ScreenManager::colorTextMuted();

  C3D_Tex *avatarTex = nullptr;
  if (dm.type == 1 && !dm.recipients.empty()) {
    const auto &r = dm.recipients[0];
    avatarTex = Discord::AvatarCache::getInstance().getAvatar(r.id, r.avatar,
                                                              r.discriminator);
  } else if (dm.type == 3 && !dm.icon.empty()) {
    avatarTex =
        Discord::AvatarCache::getInstance().getGuildIcon(dm.id, dm.icon);
  }

  if (avatarTex) {
    Tex3DS_SubTexture subtex = {
        (u16)avatarTex->width, (u16)avatarTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {avatarTex, &subtex};
    C2D_DrawImageAt(img, 18.0f, y + 8.0f, 0.5f, nullptr,
                    32.0f / avatarTex->width, 32.0f / avatarTex->height);
  } else {
    std::string iconPath = "romfs:/discord-icons/chat.png";
    C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage(iconPath);
    if (tex) {
      Tex3DS_SubTexture subtex = {
          (u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
      C2D_Image img = {tex, &subtex};
      C2D_ImageTint tint;
      C2D_PlainImageTint(&tint, textColor, 1.0f);
      C2D_DrawImageAt(img, 20.0f, y + 12.0f, 0.5f, &tint, 24.0f / tex->width,
                      24.0f / tex->height);
    }
  }

  std::string dispName = dm.name;
  if (dispName.empty() && dm.type == 1 && !dm.recipients.empty()) {
    dispName = dm.recipients[0].global_name;
    if (dispName.empty()) {
      dispName = dm.recipients[0].username;
    }
  }
  float maxW = 390.0f - 60.0f - 10.0f;
  if (measureRichText(dispName, 0.55f, 0.55f) > maxW) {
    while (!dispName.empty() &&
           measureRichText(dispName + "...", 0.55f, 0.55f) > maxW) {
      if ((dispName.back() & 0xc0) == 0x80) {
        while (!dispName.empty() && (dispName.back() & 0xc0) == 0x80)
          dispName.pop_back();
      }
      if (!dispName.empty())
        dispName.pop_back();
    }
    dispName += "...";
  }

  drawRichText(60.0f, y + 14.5f, 0.5f, 0.55f, 0.55f, textColor, dispName);
}

} // namespace UI
