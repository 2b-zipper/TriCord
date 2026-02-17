#include "ui/hamburger_menu.h"
#include "core/config.h"
#include "core/i18n.h"
#include "core/log.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "ui/server_list_screen.h"
#include <3ds.h>
#include <citro2d.h>
#include <vector>

namespace UI {

const float HamburgerMenu::MENU_WIDTH = 200.0f;
const float HamburgerMenu::ANIMATION_SPEED = 0.2f;

HamburgerMenu::HamburgerMenu()
    : state(State::CLOSED), slideProgress(0.0f), selectedIndex(0),
      accountSelectionIndex(0), accountScrollOffset(0) {
  refreshStrings();
}

void HamburgerMenu::refreshStrings() {
  items.clear();
  items.push_back({TR("menu.direct_messages"), MenuItemType::DIRECT_MESSAGES});
  items.push_back({TR("menu.servers"), MenuItemType::SERVER_LIST});
  items.push_back({TR("menu.settings"), MenuItemType::SETTINGS});
}

void HamburgerMenu::toggle() {
  if (state == State::CLOSED || state == State::CLOSING) {
    open();
  } else {
    close();
  }
}

void HamburgerMenu::open() {
  if (state != State::OPEN) {
    state = State::OPENING;
    Logger::log("Opening Hamburger Menu");
  }
}

void HamburgerMenu::close() {
  if (state != State::CLOSED) {
    state = State::CLOSING;
    if (state != State::ACCOUNT_SELECTION) {
      accountSelectionIndex = 0;
      accountScrollOffset = 0;
    }
    Logger::log("Closing Hamburger Menu");
  }
}

void HamburgerMenu::reset() {
  state = State::CLOSED;
  slideProgress = 0.0f;
  selectedIndex = 0;
  accountCardSelected = false;
  accountSelectionIndex = 0;
  accountScrollOffset = 0;
}

void HamburgerMenu::update() {
  if (state == State::OPENING) {
    slideProgress += ANIMATION_SPEED;
    if (slideProgress >= 1.0f) {
      slideProgress = 1.0f;
      state = State::OPEN;
    }
  } else if (state == State::CLOSING) {
    slideProgress -= ANIMATION_SPEED;
    if (slideProgress <= 0.0f) {
      slideProgress = 0.0f;
      state = State::CLOSED;
    }
  }

  if (state == State::OPEN) {
    u32 kDown = hidKeysDown();

    if (kDown & KEY_DOWN) {
      if (!accountCardSelected) {
        if (selectedIndex < (int)items.size() - 1)
          selectedIndex++;
        else
          accountCardSelected = true;
      }
    } else if (kDown & KEY_UP) {
      if (accountCardSelected) {
        accountCardSelected = false;
      } else if (selectedIndex > 0) {
        selectedIndex--;
      }
    } else if (kDown & KEY_A) {
      if (accountCardSelected) {
        state = State::ACCOUNT_SELECTION;
        accountSelectionIndex = Config::getInstance().getCurrentAccountIndex();
        if (accountSelectionIndex < 0)
          accountSelectionIndex = 0;

        if (accountSelectionIndex >= 4) {
          accountScrollOffset = accountSelectionIndex - 3;
        } else {
          accountScrollOffset = 0;
        }
        return;
      } else {
        const auto &item = items[selectedIndex];
        Logger::log("Menu selected: %s", item.label.c_str());

        ScreenManager &sm = ScreenManager::getInstance();
        switch (item.type) {
        case MenuItemType::SERVER_LIST:
          sm.setScreen(ScreenType::GUILD_LIST);
          close();
          return;
        case MenuItemType::DIRECT_MESSAGES:
          sm.setScreen(ScreenType::DM_LIST);
          close();
          return;
        case MenuItemType::SETTINGS:
          sm.setScreen(ScreenType::SETTINGS);
          close();
          return;
        default:
          close();
          return;
        }
      }
    } else if (kDown & KEY_Y) {
      if (accountCardSelected) {
        state = State::STATUS_SELECTION;
        Discord::User self =
            Discord::DiscordClient::getInstance().getCurrentUser();
        switch (self.status) {
        case Discord::UserStatus::ONLINE:
          statusSelectionIndex = 0;
          break;
        case Discord::UserStatus::IDLE:
          statusSelectionIndex = 1;
          break;
        case Discord::UserStatus::DND:
          statusSelectionIndex = 2;
          break;
        case Discord::UserStatus::INVISIBLE:
          statusSelectionIndex = 3;
          break;
        default:
          statusSelectionIndex = 0;
          break;
        }
        return;
      }
    } else if (kDown & KEY_B) {
      close();
    } else if (kDown & KEY_RIGHT) {
      close();
    }
  } else if (state == State::ACCOUNT_SELECTION) {
    u32 kDown = hidKeysDown();
    const auto &accounts = Config::getInstance().getAccounts();
    int totalItems = (int)accounts.size() + 1;

    if (kDown & KEY_DOWN) {
      if (accountSelectionIndex < totalItems - 1) {
        accountSelectionIndex++;
        if (accountSelectionIndex - accountScrollOffset >= 4) {
          accountScrollOffset = accountSelectionIndex - 3;
        }
      }
    } else if (kDown & KEY_UP) {
      if (accountSelectionIndex > 0) {
        accountSelectionIndex--;
        if (accountSelectionIndex < accountScrollOffset) {
          accountScrollOffset = accountSelectionIndex;
        }
      }
    } else if (kDown & KEY_B) {
      state = State::OPEN;
    } else if (kDown & KEY_A) {
      if (accountSelectionIndex < (int)accounts.size()) {
        int currentIndex = Config::getInstance().getCurrentAccountIndex();
        if (accountSelectionIndex != currentIndex) {
          Logger::log("Switching to account index %d", accountSelectionIndex);
          Config::getInstance().selectAccount(accountSelectionIndex);

          Discord::DiscordClient::getInstance().disconnect();

          UI::ImageManager::getInstance().clear();
          Discord::AvatarCache::getInstance().clear();

          ScreenManager::getInstance().setSelectedGuildId("");
          ScreenManager::getInstance().setLastServerIndex(0);
          ScreenManager::getInstance().setLastServerScroll(0);

          ScreenManager::getInstance().setScreen(ScreenType::LOGIN);
        }
        close();
      } else {
        Logger::log("Adding new account requested");

        ScreenManager::getInstance().setScreen(ScreenType::ADD_ACCOUNT);
        close();
      }
    } else if (kDown & KEY_X) {
      if (accountSelectionIndex < (int)accounts.size()) {
        state = State::DELETE_CONFIRMATION;
      }
    }
  } else if (state == State::DELETE_CONFIRMATION) {
    u32 kDown = hidKeysDown();
    if (kDown & KEY_A) {
      const auto &accounts = Config::getInstance().getAccounts();
      if (accountSelectionIndex < (int)accounts.size()) {
        Logger::log("[HamburgerMenu] Deleting account %d",
                    accountSelectionIndex);
        Config::getInstance().removeAccount(accountSelectionIndex);

        // Adjust selection
        const auto &newAccounts = Config::getInstance().getAccounts();

        if (newAccounts.empty()) {
          Discord::DiscordClient::getInstance().disconnect();
          ScreenManager::getInstance().setScreen(ScreenType::LOGIN);
          close();
          return;
        }

        if (accountSelectionIndex >= (int)newAccounts.size() + 1) {
          accountSelectionIndex = (int)newAccounts.size();
        }
      }
      state = State::ACCOUNT_SELECTION;
    } else if (kDown & KEY_B) {
      state = State::ACCOUNT_SELECTION;
    }
  } else if (state == State::STATUS_SELECTION) {
    u32 kDown = hidKeysDown();
    if (kDown & KEY_DOWN) {
      if (statusSelectionIndex < 3)
        statusSelectionIndex++;
    } else if (kDown & KEY_UP) {
      if (statusSelectionIndex > 0)
        statusSelectionIndex--;
    } else if (kDown & KEY_B) {
      state = State::OPEN;
    } else if (kDown & KEY_A) {
      Discord::UserStatus newStatus = Discord::UserStatus::ONLINE;
      switch (statusSelectionIndex) {
      case 0:
        newStatus = Discord::UserStatus::ONLINE;
        break;
      case 1:
        newStatus = Discord::UserStatus::IDLE;
        break;
      case 2:
        newStatus = Discord::UserStatus::DND;
        break;
      case 3:
        newStatus = Discord::UserStatus::INVISIBLE;
        break;
      }
      Discord::DiscordClient::getInstance().updatePresence(newStatus);
      state = State::OPEN;
    }
  }
}

void HamburgerMenu::render() {
  if (state == State::CLOSED)
    return;

  float x = (slideProgress * MENU_WIDTH) - MENU_WIDTH;
  float alpha = slideProgress;

  drawOverlay(0.96f);

  u32 menuBg = ScreenManager::colorBackgroundDark();
  u8 r = (menuBg >> 0) & 0xFF;
  u8 g = (menuBg >> 8) & 0xFF;
  u8 b = (menuBg >> 16) & 0xFF;
  u32 glassBg = C2D_Color32(r, g, b, 240);

  C2D_DrawRectSolid(x, 0, 0.97f, MENU_WIDTH, 240, glassBg);
  C2D_DrawRectSolid(x + MENU_WIDTH - 1, 0, 0.975f, 1, 240,
                    C2D_Color32(255, 255, 255, 30));

  if (state == State::OPEN || state == State::OPENING ||
      state == State::CLOSING) {
    float y = 10.0f;
    for (size_t i = 0; i < items.size(); i++) {
      drawMenuItem(i, y, alpha);
      y += 40.0f;
    }

    drawAccountCard(x, 240.0f - 50.0f, alpha);
  } else if (state == State::ACCOUNT_SELECTION) {
    float popupW = 280.0f;
    float popupH = 200.0f;
    float popupX = (400.0f - popupW) / 2.0f;
    float popupY = (240.0f - popupH) / 2.0f;

    drawPopupBackground(popupX, popupY, popupW, popupH, 0.98f);

    float padding = 10.0f;
    drawText(popupX + padding, popupY + padding, 0.99f, 0.6f, 0.6f,
             ScreenManager::colorText(),
             Core::I18n::getInstance().get("menu.select_account"));

    const auto &accounts = Config::getInstance().getAccounts();
    int maxVisible = 4;
    float itemStartDim = popupY + 40.0f;

    for (int i = 0; i < (int)accounts.size() + 1; i++) {
      if (i < accountScrollOffset || i >= accountScrollOffset + maxVisible)
        continue;

      float itemY = itemStartDim + (i - accountScrollOffset) * 34.0f;
      u32 color = ScreenManager::colorTextMuted();

      bool isSelected = i == accountSelectionIndex;
      u32 selCol = (i < (int)accounts.size()) ? ScreenManager::colorSelection()
                                              : ScreenManager::colorSuccess();
      drawPopupMenuItem(popupX + 5, itemY, popupW - 10, 30.0f, 0.985f,
                        isSelected, selCol);

      if (isSelected) {
        color = ScreenManager::colorWhite();
      }

      if (i < (int)accounts.size()) {
        std::string label = accounts[i].name;
        if (i == Config::getInstance().getCurrentAccountIndex()) {
          label += TR("menu.active");
        }
        drawText(popupX + 10, itemY + 5, 0.99f, 0.5f, 0.5f, color, label);
      } else {

        drawText(popupX + 10, itemY + 5, 0.99f, 0.5f, 0.5f, color,
                 "+ " + Core::I18n::getInstance().get("menu.add_account"));
      }
    }

    if (accountScrollOffset > 0) {
      drawText(popupX + popupW - 25, popupY + 38, 0.99f, 0.4f, 0.4f,
               ScreenManager::colorTextMuted(), "\u25B2");
    }
    if ((int)accounts.size() + 1 > accountScrollOffset + maxVisible) {
      drawText(popupX + popupW - 25, popupY + popupH - 42, 0.99f, 0.4f, 0.4f,
               ScreenManager::colorTextMuted(), "\u25BC");
    }

    drawText(popupX + 10, popupY + popupH - 20, 0.99f, 0.4f, 0.4f,
             ScreenManager::colorTextMuted(),
             "\uE000: " + TR("common.select") + "  \uE002: " +
                 TR("common.delete") + "  \uE001: " + TR("common.back"));
  } else if (state == State::DELETE_CONFIRMATION) {
    const auto &accounts = Config::getInstance().getAccounts();
    std::string accName = (accountSelectionIndex < (int)accounts.size())
                              ? accounts[accountSelectionIndex].name
                              : "Account";

    float cw = 280.0f;
    float ch = 110.0f;
    float cx = (400.0f - cw) / 2.0f;
    float cy = (240.0f - ch) / 2.0f;

    drawOverlay(0.99f);
    drawPopupBackground(cx, cy, cw, ch, 0.995f, 12.0f);

    std::string confirmMsg =
        Core::I18n::format(TR("menu.delete_confirm"), accName);
    drawText(cx + 10, cy + 12.0f, 0.997f, 0.5f, 0.5f,
             ScreenManager::colorText(), confirmMsg);

    drawText(cx + 10, cy + 42.0f, 0.997f, 0.45f, 0.45f,
             ScreenManager::colorTextMuted(), TR("menu.delete_warning"));

    drawText(cx + 10, cy + ch - 22.0f, 0.997f, 0.4f, 0.4f,
             ScreenManager::colorError(),
             "\uE000: " + TR("common.delete") +
                 "  \uE001: " + TR("common.cancel"));
  } else if (state == State::STATUS_SELECTION) {
    float popupW = 200.0f;
    float popupH = 142.0f;
    float popupX = (400.0f - popupW) / 2.0f;
    float popupY = (240.0f - popupH) / 2.0f;

    drawPopupBackground(popupX, popupY, popupW, popupH, 0.98f);

    float padding = 8.0f;
    drawText(popupX + padding, popupY + padding, 0.99f, 0.6f, 0.6f,
             ScreenManager::colorText(), TR("menu.status_change"));

    float itemStartDim = popupY + 34.0f;
    for (int i = 0; i < 4; i++) {
      float itemY = itemStartDim + i * 26.0f;
      u32 color = ScreenManager::colorTextMuted();

      bool isSelected = i == statusSelectionIndex;
      drawPopupMenuItem(popupX + 5, itemY, popupW - 10, 24.0f, 0.985f,
                        isSelected, ScreenManager::colorSelection());

      if (isSelected) {
        color = ScreenManager::colorWhite();
      }

      std::string label = "";
      std::string iconPath = "romfs:/discord-icons/status/offline.png";
      switch (i) {
      case 0:
        label = TR("status.online");
        iconPath = "romfs:/discord-icons/status/online.png";
        break;
      case 1:
        label = TR("status.idle");
        iconPath = "romfs:/discord-icons/status/idle.png";
        break;
      case 2:
        label = TR("status.dnd");
        iconPath = "romfs:/discord-icons/status/donotdisturb.png";
        break;
      case 3:
        label = TR("status.invisible");
        iconPath = "romfs:/discord-icons/status/offline.png";
        break;
      }

      C3D_Tex *statTex = ImageManager::getInstance().getLocalImage(iconPath);
      if (statTex) {
        float sSize = 12.0f;
        Tex3DS_SubTexture subtex = {
            (u16)statTex->width, (u16)statTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
        C2D_Image img = {statTex, &subtex};
        C2D_DrawImageAt(img, popupX + 12, itemY + 11 - (sSize / 2), 0.99f,
                        nullptr, sSize / statTex->width,
                        sSize / statTex->height);
      }
      drawText(popupX + 30, itemY + 3, 0.99f, 0.45f, 0.45f, color, label);
    }
  }
}

void HamburgerMenu::drawMenuItem(int index, float y, float alpha) {
  float x = (slideProgress * MENU_WIDTH) - MENU_WIDTH;

  u32 textColor = ScreenManager::colorText();
  if (index == selectedIndex && !accountCardSelected) {
    u32 highlight = ScreenManager::colorSelection();
    drawRoundedRect(x + 8, y, 0.975f, MENU_WIDTH - 16, 32, 8.0f, highlight);
    textColor = ScreenManager::colorWhite();
  } else {
    u32 base = ScreenManager::colorText();
    u8 r = base & 0xFF;
    u8 g = (base >> 8) & 0xFF;
    u8 b = (base >> 16) & 0xFF;
    textColor = C2D_Color32(r, g, b, (int)(255 * alpha));
  }

  drawText(x + 20.0f, y + 5.0f, 0.98f, 0.6f, 0.6f, textColor,
           items[index].label);
}

void HamburgerMenu::drawAccountCard(float x, float y, float alpha) {
  Discord::User self = Discord::DiscordClient::getInstance().getCurrentUser();
  float cardH = 55.0f;

  // Highlight if selected
  if (accountCardSelected) {
    drawRoundedRect(x + 8, y, 0.975f, MENU_WIDTH - 16, cardH - 5, 8.0f,
                    ScreenManager::colorSelection());
  } else {
    // Subtle separator line
    C2D_DrawRectSolid(x + 10, y, 0.975f, MENU_WIDTH - 20, 1.0f,
                      ScreenManager::colorBackgroundLight());
  }

  float avatarX = x + 10.0f;
  float avatarY = y + 10.0f;
  float avatarSize = 30.0f;

  C3D_Tex *avatarTex = Discord::AvatarCache::getInstance().getAvatar(
      self.id, self.avatar, self.discriminator);
  if (avatarTex) {
    Tex3DS_SubTexture subtex = {
        (u16)avatarTex->width, (u16)avatarTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {avatarTex, &subtex};
    C2D_DrawImageAt(img, avatarX, avatarY, 0.98f, nullptr,
                    avatarSize / avatarTex->width,
                    avatarSize / avatarTex->height);
  } else {
    Discord::AvatarCache::getInstance().prefetchAvatar(self.id, self.avatar,
                                                       self.discriminator);
    C2D_DrawRectSolid(avatarX, avatarY, 0.98f, avatarSize, avatarSize,
                      ScreenManager::colorBackgroundLight());
    std::string initial =
        self.username.empty() ? "?" : self.username.substr(0, 1);
    drawText(avatarX + 10, avatarY + 6, 0.99f, 0.45f, 0.45f,
             ScreenManager::colorWhite(), initial);
  }

  u32 textColor = accountCardSelected ? ScreenManager::colorWhite()
                                      : ScreenManager::colorText();
  drawText(avatarX + avatarSize + 10.0f, y + 10.0f, 0.98f, 0.45f, 0.45f,
           textColor, self.username);

  u32 statusTextColor = accountCardSelected ? ScreenManager::colorWhite()
                                            : ScreenManager::colorTextMuted();

  std::string statusStr = "";
  std::string iconPath = "romfs:/discord-icons/status/offline.png";
  switch (self.status) {
  case Discord::UserStatus::ONLINE:
    statusStr = TR("status.online");
    iconPath = "romfs:/discord-icons/status/online.png";
    break;
  case Discord::UserStatus::IDLE:
    statusStr = TR("status.idle");
    iconPath = "romfs:/discord-icons/status/idle.png";
    break;
  case Discord::UserStatus::DND:
    statusStr = TR("status.dnd");
    iconPath = "romfs:/discord-icons/status/donotdisturb.png";
    break;
  case Discord::UserStatus::INVISIBLE:
    statusStr = TR("status.invisible");
    iconPath = "romfs:/discord-icons/status/offline.png";
    break;
  case Discord::UserStatus::OFFLINE:
    statusStr = "Offline";
    iconPath = "romfs:/discord-icons/status/offline.png";
    break;
  default:
    statusStr = "Unknown";
    iconPath = "romfs:/discord-icons/status/offline.png";
    break;
  }

  // Status Icon
  C3D_Tex *statTex = ImageManager::getInstance().getLocalImage(iconPath);
  if (statTex) {
    float sSize = 10.0f;
    Tex3DS_SubTexture subtex = {
        (u16)statTex->width, (u16)statTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {statTex, &subtex};

    // Draw background circle for the icon to stand out
    C2D_DrawCircleSolid(avatarX + avatarSize - 2, avatarY + avatarSize - 2,
                        0.985f, 6, ScreenManager::colorBackgroundDark());

    C2D_DrawImageAt(img, avatarX + avatarSize - 2 - (sSize / 2),
                    avatarY + avatarSize - 2 - (sSize / 2), 0.99f, nullptr,
                    sSize / statTex->width, sSize / statTex->height);
  }

  drawText(avatarX + avatarSize + 10.0f, y + 28.0f, 0.98f, 0.35f, 0.35f,
           statusTextColor, statusStr);

  if (accountCardSelected) {
    drawText(x + MENU_WIDTH - 25, y + 19, 0.98f, 0.4f, 0.4f,
             ScreenManager::colorWhite(), "\uE003");
  }
}

} // namespace UI
