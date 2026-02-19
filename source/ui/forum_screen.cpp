#include "ui/forum_screen.h"
#include "core/i18n.h"
#include "log.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include <algorithm>
#include <set>

namespace UI {

static std::set<std::string> forumPendingMemberFetches;

ForumScreen::ForumScreen(const std::string &channelId,
                         const std::string &channelName)
    : channelId(channelId), channelName(channelName), guildId(""), threads({}),
      activeThreadCount(0), repeatTimer(0), lastKey(0), isLoading(true) {
  auto &sm = ScreenManager::getInstance();
  selectedIndex = sm.getLastForumIndex(channelId);
  scrollOffset = sm.getLastForumScroll(channelId);

  truncatedChannelName =
      getTruncatedRichText(channelName, 380.0f, 0.52f, 0.52f);
}

ForumScreen::~ForumScreen() {}

void ForumScreen::onEnter() {
  Logger::log("Entered Forum Screen: %s", channelName.c_str());
  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  guildId = client.getGuildIdFromChannel(channelId);

  std::lock_guard<std::recursive_mutex> lock(client.getMutex());
  Discord::Channel channel = client.getChannel(channelId);
  channelTopic = channel.topic;

  truncatedChannelName =
      getTruncatedRichText(channelName, 380.0f, 0.52f, 0.52f);

  fetchThreads();
}

void ForumScreen::onExit() {}

void ForumScreen::fetchThreads() {
  isLoading = true;
  Discord::DiscordClient::getInstance().fetchForumThreads(
      channelId, [this](const std::vector<Discord::Channel> &fetchedThreads) {
        std::vector<Discord::Channel> active;
        std::vector<Discord::Channel> archived;
        for (const auto &t : fetchedThreads) {
          if (t.is_archived)
            archived.push_back(t);
          else
            active.push_back(t);
        }

        auto sortThreads = [](const Discord::Channel &a,
                              const Discord::Channel &b) {
          bool aPinned = (a.flags & 2);
          bool bPinned = (b.flags & 2);

          if (aPinned != bPinned)
            return aPinned;

          if (a.last_message_id.length() != b.last_message_id.length())
            return a.last_message_id.length() > b.last_message_id.length();
          return a.last_message_id > b.last_message_id;
        };

        std::sort(active.begin(), active.end(), sortThreads);
        std::sort(archived.begin(), archived.end(), sortThreads);

        this->activeThreadCount = (int)active.size();

        std::vector<Discord::Channel> allChannels = std::move(active);
        allChannels.insert(allChannels.end(), archived.begin(), archived.end());

        this->threads.clear();
        for (const auto &ch : allChannels) {
          ThreadInfo info;
          info.channel = ch;
          this->threads.push_back(info);
        }

        this->isLoading = false;
      });
}

void ForumScreen::update() {
  if (isLoading)
    return;

  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();

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

  if (moveDir & KEY_DOWN) {
    if (selectedIndex < (int)threads.size() - 1) {
      selectedIndex++;

      float fullItemHeight = CARD_HEIGHT + 5.0f;
      float sepHeight = 25.0f;

      float selectedY = 5.0f;
      for (int i = 0; i < selectedIndex; ++i) {
        if (i == activeThreadCount && activeThreadCount > 0 &&
            activeThreadCount < (int)threads.size()) {
          selectedY += sepHeight;
        }
        selectedY += fullItemHeight;
      }

      int visibleItems = (int)(240.0f / fullItemHeight);
      if (selectedIndex >= scrollOffset + visibleItems) {
        scrollOffset++;
      }

      auto &sm = ScreenManager::getInstance();
      sm.setLastForumIndex(channelId, selectedIndex);
      sm.setLastForumScroll(channelId, scrollOffset);
    }
  } else if (moveDir & KEY_UP) {
    if (selectedIndex > 0) {
      selectedIndex--;
      if (selectedIndex < scrollOffset) {
        scrollOffset = selectedIndex;
      }

      auto &sm = ScreenManager::getInstance();
      sm.setLastForumIndex(channelId, selectedIndex);
      sm.setLastForumScroll(channelId, scrollOffset);
    }
  }

  if (kDown & KEY_A) {
    if (selectedIndex >= 0 && selectedIndex < (int)threads.size()) {
      const auto &thread = threads[selectedIndex].channel;
      Discord::DiscordClient::getInstance().setSelectedChannelId(thread.id);
      ScreenManager::getInstance().pushScreen(ScreenType::MESSAGES);
    }
  }

  if (kDown & KEY_B) {
    ScreenManager::getInstance().returnToPreviousScreen();
  }
}

void ForumScreen::renderTop(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackground());

  const float headerH = 26.0f;

  if (isLoading) {
    drawCenteredText(120.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
                     TR("forum.loading"), 400.0f);
    return;
  }

  if (threads.empty()) {
    drawCenteredText(120.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
                     TR("forum.no_threads"), 400.0f);
    return;
  }

  float startY = headerH + 5.0f;
  float currentY = startY;
  float fullItemHeight = CARD_HEIGHT + 5.0f;
  float sepHeight = 25.0f;

  u32 glassCol = ScreenManager::colorHeaderGlass();
  C2D_DrawRectSolid(0, 0, 0.9f, 400.0f, headerH, glassCol);
  drawRoundedRect(0, headerH - 1.0f, 0.91f, 400.0f, 1.0f, 0.5f,
                  ScreenManager::colorHeaderBorder());
  drawCenteredRichText(4.0f, 0.95f, 0.52f, 0.52f, ScreenManager::colorText(),
                       truncatedChannelName, 400.0f);

  for (int i = scrollOffset; i < (int)threads.size(); ++i) {
    if (i == activeThreadCount && activeThreadCount > 0 &&
        activeThreadCount < (int)threads.size()) {
      u32 sepColor = ScreenManager::colorSeparator();
      C2D_DrawRectSolid(10.0f, currentY + 14.0f, 0.5f, 380.0f, 1.0f, sepColor);
      drawText(15.0f, currentY + 4.0f, 0.5f, 0.4f, 0.4f, sepColor,
               TR("forum.archived"));
      currentY += sepHeight;
    }

    renderThreadCard(i, currentY);
    currentY += fullItemHeight;

    if (currentY > 240.0f)
      break;
  }
}

void ForumScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

  float headerX = 35.0f;

  std::string iconPath = "romfs:/discord-icons/forum.png";
  C3D_Tex *icon = UI::ImageManager::getInstance().getLocalImage(iconPath);
  if (icon) {
    float iconSize = 18.0f;
    Tex3DS_SubTexture subtex = {
        (u16)icon->width, (u16)icon->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {icon, &subtex};

    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);

    C2D_DrawImageAt(img, 35.0f, 9.0f, 0.5f, &tint, iconSize / icon->width,
                    iconSize / icon->height);
    headerX = 35.0f + iconSize + 5.0f;
  } else {
    drawText(35.0f, 10.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
             "#");
    headerX = 50.0f;
  }

  std::string dispNameBottom =
      getTruncatedRichText(channelName, 310.0f - headerX, 0.6f, 0.6f);

  drawRichText(headerX, 10.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorText(),
               dispNameBottom);

  C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());

  std::string displayTopic =
      channelTopic.empty() ? TR("common.no_topic") : channelTopic;

  float topicY = 40.0f;

  drawText(10.0f, topicY, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(),
           TR("forum.topic_label"));
  topicY += 15.0f;

  auto lines = MessageUtils::wrapText(displayTopic, 300.0f, 0.4f);
  int lineCount = 0;

  for (const auto &line : lines) {
    if (lineCount >= 12)
      break;

    drawRichText(10.0f, topicY, 0.5f, 0.4f, 0.4f, ScreenManager::colorText(),
                 line);
    topicY += 13.0f;
    lineCount++;
  }

  float controlsY = 240.0f - 25.0f;

  drawText(10.0f, controlsY, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
           "\uE079\uE07A: " + TR("common.navigate") + "  \uE000: " +
               TR("common.open") + "  \uE001: " + TR("common.back"));
}

void ForumScreen::renderThreadCard(int index, float y) {
  bool isSelected = (index == selectedIndex);
  const auto &info = threads[index];
  const auto &thread = info.channel;

  float x = (400.0f - CARD_WIDTH) / 2.0f;

  u32 bgColor = isSelected ? ScreenManager::colorBackgroundLight()
                           : ScreenManager::colorBackgroundDark();

  drawRoundedRect(x, y, 0.5f, CARD_WIDTH, CARD_HEIGHT, 10.0f, bgColor);

  if (isSelected) {
    drawRoundedRect(x + 2, y + 8, 0.6f, 3.0f, CARD_HEIGHT - 16, 1.5f,
                    ScreenManager::colorSelection());
  }

  float textX = x + 12.0f;
  float currentY = y + 8.0f;

  bool isPinned = (thread.flags & 2);
  if (isPinned) {
    std::string pinPath = "romfs:/discord-icons/pin.png";
    C3D_Tex *pinIcon = UI::ImageManager::getInstance().getLocalImage(pinPath);
    if (pinIcon) {
      float pinSize = 16.0f;
      Tex3DS_SubTexture subtex = {
          (u16)pinIcon->width, (u16)pinIcon->height, 0.0f, 1.0f, 1.0f, 0.0f};
      C2D_Image img = {pinIcon, &subtex};

      C2D_ImageTint tint;
      C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);

      C2D_DrawImageAt(img, textX, currentY + 2.0f, 0.6f, &tint,
                      pinSize / pinIcon->width, pinSize / pinIcon->height);

      textX += pinSize + 5.0f;
    }
  }

  if (!info.titleProcessed) {
    float maxTitleW = CARD_WIDTH - (textX - x) - 15.0f;
    info.truncatedTitle =
        getTruncatedRichText(thread.name, maxTitleW, 0.65f, 0.65f);
    info.titleProcessed = true;
  }
  std::string titleStr = info.truncatedTitle;
  u32 titleColor = thread.is_archived ? ScreenManager::colorTextMuted()
                                      : ScreenManager::colorText();

  drawRichText(textX, currentY, 0.6f, 0.65f, 0.65f, titleColor, titleStr);
  currentY += 20.0f;

  textX = x + 12.0f;

  if (!thread.op_content.empty()) {
    float previewX = textX;

    std::string displayName = thread.owner_name;
    u32 nameColor = ScreenManager::colorText();

    if (!thread.owner_id.empty()) {
      Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

      Discord::User tempUser;
      tempUser.id = thread.owner_id;
      tempUser.username = thread.owner_name;
      displayName =
          client.getMemberDisplayName(guildId, thread.owner_id, tempUser);

      int roleColor = client.getRoleColor(guildId, thread.owner_id);
      if (roleColor != 0) {
        nameColor = C2D_Color32((roleColor >> 16) & 0xFF,
                                (roleColor >> 8) & 0xFF, roleColor & 0xFF, 255);
      }
    }

    if (!displayName.empty()) {
      drawText(previewX, currentY, 0.5f, 0.45f, 0.45f, nameColor, displayName);
      previewX += measureText(displayName, 0.45f, 0.45f);

      std::string colon = ": ";
      drawText(previewX, currentY, 0.5f, 0.45f, 0.45f,
               ScreenManager::colorTextMuted(), colon);
      previewX += measureText(colon, 0.45f, 0.45f);
    }

    if (!info.previewProcessed) {
      std::string preview = thread.op_content;
      std::replace(preview.begin(), preview.end(), '\n', ' ');

      float maxPrevW = (x + CARD_WIDTH - 15.0f) - previewX;
      info.truncatedPreview = getTruncatedText(preview, maxPrevW, 0.45f, 0.45f);
      info.previewProcessed = true;
    }

    drawText(previewX, currentY, 0.5f, 0.45f, 0.45f,
             ScreenManager::colorTextMuted(), info.truncatedPreview);
    currentY += 15.0f;
  }

  currentY += 4.0f;

  textX = x + 12.0f;

  C3D_Tex *chatIcon = UI::ImageManager::getInstance().getLocalImage(
      "romfs:/discord-icons/chat.png");
  if (chatIcon) {
    float iconSize = 12.0f;
    Tex3DS_SubTexture subtex = {
        (u16)chatIcon->width, (u16)chatIcon->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {chatIcon, &subtex};
    C2D_DrawImageAt(img, textX, currentY + 1.0f, 0.6f, nullptr,
                    iconSize / chatIcon->width, iconSize / chatIcon->height);
    textX += iconSize + 4.0f;
  }

  std::string msgCountStr = std::to_string(thread.message_count);
  std::string timeStr = "";

  if (!thread.last_message_id.empty()) {
    time_t timestamp_sec =
        MessageUtils::snowflakeToTimestamp(thread.last_message_id);
    if (timestamp_sec > 0) {
      timeStr = MessageUtils::getRelativeTime(timestamp_sec);
    }
  }

  drawText(textX, currentY, 0.6f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
           msgCountStr);
  float statsWidth = measureText(msgCountStr, 0.45f, 0.45f);

  if (!timeStr.empty()) {
    drawText(textX + statsWidth + 15.0f, currentY, 0.6f, 0.45f, 0.45f,
             ScreenManager::colorTextMuted(), timeStr);
  }
}

} // namespace UI
