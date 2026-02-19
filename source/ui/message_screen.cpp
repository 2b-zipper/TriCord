#include "ui/message_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "log.h"
#include "ui/emoji_manager.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include <3ds.h>
#include <algorithm>
#include <citro2d.h>
#include <ctime>

#include <mutex>
#include <set>

namespace UI {

MessageScreen::MessageScreen(const std::string &channelId,
                             const std::string &channelName)
    : channelId(channelId), channelName(channelName), selectedIndex(0),
      isLoading(true), isFetchingHistory(false), requestHistoryFetch(false),
      scrollInitialized(false), showNewMessageIndicator(false),
      newMessageCount(0), isForumView(false), hasMoreHistory(true),
      keyRepeatTimer(0), targetScrollY(0.0f), currentScrollY(0.0f),
      totalContentHeight(0.0f), isMenuOpen(false), menuIndex(0) {
  aliveToken = std::make_shared<bool>(true);
  Logger::log("MessageScreen initialized for channel: %s", channelName.c_str());
}

MessageScreen::~MessageScreen() {
  *aliveToken = false;
  Discord::DiscordClient::getInstance().setMessageCallback(nullptr);
  Discord::DiscordClient::getInstance().setMessageUpdateCallback(nullptr);
  Discord::DiscordClient::getInstance().setMessageDeleteCallback(nullptr);
  Discord::DiscordClient::getInstance().setConnectionCallback(nullptr);

  embedHeightCache.clear();
  ImageManager::getInstance().clearRemote();
}

void MessageScreen::onEnter() {
  isLoading = true;
  newMessageCount = 0;

  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  std::lock_guard<std::recursive_mutex> lock(client.getMutex());
  Discord::Channel channel = client.getChannel(channelId);
  this->channelType = channel.type;
  this->channelTopic = channel.topic;
  this->guildId = client.getGuildIdFromChannel(channelId);

  this->truncatedChannelName =
      getTruncatedRichText(this->channelName, 310.0f - 56.0f, 0.55f, 0.55f);

  if (!this->guildId.empty()) {
    client.sendLazyRequest(this->guildId, channelId);
  }

  client.setMessageCallback([this](const Discord::Message &msg) {
    if (msg.channelId != channelId)
      return;

    std::lock_guard<std::recursive_mutex> lock(messageMutex);

    bool found = false;
    for (auto &m : this->messages) {
      if (m.id == msg.id) {
        m = msg;
        found = true;
        break;
      }
      if (m.id.find("pending_") == 0 && m.content == msg.content &&
          m.author.id == msg.author.id) {
        m = msg;
        found = true;
        break;
      }
    }

    if (!found) {

      const float SCREEN_HEIGHT = 230.0f;
      float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
      bool wasAtBottom = (targetScrollY >= maxScroll - 5.0f);

      this->messages.push_back(msg);
      rebuildLayoutCache();

      if (wasAtBottom) {
        selectedIndex = this->messages.size() - 1;
        scrollToBottom();
      } else {
        showNewMessageIndicator = true;
        newMessageCount++;
      }
    } else {
      rebuildLayoutCache();
    }
  });

  client.setMessageUpdateCallback([this](const Discord::Message &msg) {
    if (msg.channelId != channelId)
      return;
    std::lock_guard<std::recursive_mutex> lock(messageMutex);
    bool found = false;
    for (auto &m : this->messages) {
      if (m.id == msg.id) {
        m.content = msg.content;
        m.edited_timestamp = msg.edited_timestamp;
        m.embeds = msg.embeds;
        m.attachments = msg.attachments;
        found = true;
        break;
      }
    }
    if (found) {
      const float SCREEN_HEIGHT = 230.0f;
      float oldMaxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
      bool wasAtBottom = (targetScrollY >= oldMaxScroll - 5.0f);

      rebuildLayoutCache();

      if (wasAtBottom) {
        scrollToBottom();
      }
    }
  });

  client.setMessageDeleteCallback([this](const std::string &msgId) {
    std::lock_guard<std::recursive_mutex> lock(messageMutex);
    for (size_t i = 0; i < this->messages.size(); i++) {
      if (this->messages[i].id == msgId) {
        this->messages.erase(this->messages.begin() + i);
        if (selectedIndex >= (int)this->messages.size()) {
          selectedIndex = std::max(0, (int)this->messages.size() - 1);
        }
        break;
      }
    }
    rebuildLayoutCache();
  });

  client.setMessageReactionAddCallback([this](const std::string &channelId,
                                              const std::string &messageId,
                                              const std::string &userId,
                                              const Discord::Emoji &emoji) {
    if (channelId != this->channelId)
      return;
    std::lock_guard<std::recursive_mutex> lock(messageMutex);
    for (auto &msg : this->messages) {
      if (msg.id == messageId) {
        bool found = false;
        bool isMe = (userId ==
                     Discord::DiscordClient::getInstance().getCurrentUser().id);

        for (auto &r : msg.reactions) {
          if (r.emoji.id == emoji.id && r.emoji.name == emoji.name) {
            r.count++;
            if (isMe)
              r.me = true;
            found = true;
            break;
          }
        }
        if (!found) {
          Discord::Reaction newR;
          newR.emoji = emoji;
          newR.count = 1;
          newR.me = isMe;
          msg.reactions.push_back(newR);
        }

        const float SCREEN_HEIGHT = 230.0f;
        float oldMaxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
        bool wasAtBottom = (targetScrollY >= oldMaxScroll - 5.0f);

        rebuildLayoutCache();

        if (wasAtBottom) {
          scrollToBottom();
        }
        break;
      }
    }
  });

  client.setMessageReactionRemoveCallback([this](const std::string &channelId,
                                                 const std::string &messageId,
                                                 const std::string &userId,
                                                 const Discord::Emoji &emoji) {
    if (channelId != this->channelId)
      return;
    std::lock_guard<std::recursive_mutex> lock(messageMutex);
    for (auto &msg : this->messages) {
      if (msg.id == messageId) {
        bool isMe = (userId ==
                     Discord::DiscordClient::getInstance().getCurrentUser().id);

        for (auto it = msg.reactions.begin(); it != msg.reactions.end(); ++it) {
          if (it->emoji.id == emoji.id && it->emoji.name == emoji.name) {
            it->count--;
            if (isMe)
              it->me = false;

            if (it->count <= 0) {
              msg.reactions.erase(it);
            }

            const float SCREEN_HEIGHT = 230.0f;
            float oldMaxScroll =
                std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
            bool wasAtBottom = (targetScrollY >= oldMaxScroll - 5.0f);

            rebuildLayoutCache();

            if (wasAtBottom) {
              scrollToBottom();
            }
            break;
          }
        }
        break;
      }
    }
  });

  client.setConnectionCallback([this]() {
    Logger::log("[UI] Gateway reconnected, catching up messages...");
    catchUpMessages();
  });

  if (!channel.name.empty() && channel.name != "Channel") {
    channelName = channel.name;
  }

  this->messages.clear();
  isForumView = (channel.type == 15);

  if (isForumView) {
    client.fetchForumThreads(
        channelId, [this, token = aliveToken](
                       const std::vector<Discord::Channel> &threads) {
          if (!*token)
            return;
          std::vector<Discord::Message> threadMsgs;
          for (const auto &t : threads) {
            Discord::Message m;
            m.id = t.id;
            m.content = t.name;
            m.author.username = TR("message.thread");
            m.type = t.type;
            m.timestamp = "";
            threadMsgs.push_back(m);
          }

          {
            std::lock_guard<std::recursive_mutex> lock(messageMutex);
            this->messages = threadMsgs;
            rebuildLayoutCache();
            if (!this->messages.empty()) {
              selectedIndex = 0;
            }
          }
          isLoading = false;
        });
    return;
  }

  client.fetchMessagesAsync(
      channelId, 25,
      [this, token = aliveToken](const std::vector<Discord::Message> &fetched) {
        if (!*token)
          return;
        if (fetched.empty()) {
          isLoading = false;
          return;
        }

        std::vector<Discord::Message> reversed = fetched;
        std::reverse(reversed.begin(), reversed.end());

        {
          std::lock_guard<std::recursive_mutex> lock(messageMutex);

          if (this->messages.empty()) {
            this->messages = reversed;
            rebuildLayoutCache();
            selectedIndex = this->messages.size() - 1;
            scrollToBottom();
          } else {
            float oldTotalH = totalContentHeight;
            this->messages.insert(this->messages.begin(), reversed.begin(),
                                  reversed.end());
            selectedIndex += reversed.size();
            rebuildLayoutCache();

            float hDiff = totalContentHeight - oldTotalH;
            currentScrollY += hDiff;
            targetScrollY += hDiff;
          }
        }

        isLoading = false;
        Logger::log("MessageScreen loaded %d messages async via NetworkManager",
                    reversed.size());
      });
}

void MessageScreen::update() {
  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  std::lock_guard<std::recursive_mutex> clientLock(client.getMutex());
  std::lock_guard<std::recursive_mutex> updateLock(messageMutex);
  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();

  if (kDown & KEY_TOUCH) {
    touchPosition touch;
    hidTouchRead(&touch);

    float btnW = 30.0f;
    float btnH = 30.0f;
    float btnX = 320.0f - btnW - 10.0f;
    float btnY = 240.0f - btnH - 10.0f;

    if (touch.px >= btnX && touch.px <= btnX + btnW && touch.py >= btnY &&
        touch.py <= btnY + btnH) {
      if (!isMenuOpen && !isLoading) {
        scrollToBottom();
      }
    }
  }

  if ((kDown & KEY_B) && !isMenuOpen) {
    Discord::DiscordClient::getInstance().setMessageCallback(nullptr);
    Discord::DiscordClient::getInstance().setMessageDeleteCallback(nullptr);
    Discord::DiscordClient::getInstance().setMessageReactionAddCallback(
        nullptr);
    Discord::DiscordClient::getInstance().setMessageReactionRemoveCallback(
        nullptr);

    Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
    {
      std::lock_guard<std::recursive_mutex> lock(client.getMutex());
      Discord::Channel channel = client.getChannel(channelId);
      if (!channel.parent_id.empty()) {
        Discord::Channel parent = client.getChannel(channel.parent_id);
        if (parent.type == 15) {
          client.setSelectedChannelId(channel.parent_id);
        }
      }
    }

    ScreenManager::getInstance().returnToPreviousScreen();
    return;
  }

  if (isLoading)

    std::lock_guard<std::recursive_mutex> lock(messageMutex);
  if (isMenuOpen) {
    if (kDown & KEY_DOWN) {
      if (menuIndex < (int)menuOptions.size() - 1)
        menuIndex++;
    }
    if (kDown & KEY_UP) {
      if (menuIndex > 0)
        menuIndex--;
    }

    if (kDown & KEY_B) {
      isMenuOpen = false;
    }

    if (kDown & KEY_A) {
      if (menuIndex < 0 || menuIndex >= (int)menuActions.size()) {
        isMenuOpen = false;
        return;
      }

      std::string action = menuActions[menuIndex];
      isMenuOpen = false;

      if (action == "Cancel") {
      } else if (action == "Reply") {
        if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
          const auto &msg = messages[selectedIndex];
          SwkbdState swkbd;
          char mybuf[2000];
          mybuf[0] = '\0';
          swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
          swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT |
                                       SWKBD_DARKEN_TOP_SCREEN |
                                       SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET |
                                       SWKBD_ALLOW_POWER | SWKBD_MULTILINE);
          swkbdSetHintText(&swkbd, TR("common.reply_hint").c_str());
          swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, TR("common.cancel").c_str(),
                         false);
          swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, TR("common.send").c_str(),
                         true);
          SwkbdButton button = swkbdInputText(&swkbd, mybuf, sizeof(mybuf));

          if (button == SWKBD_BUTTON_RIGHT) {
            std::string content = mybuf;
            size_t first = content.find_first_not_of(" \n\r\t");
            if (first != std::string::npos) {
              content = content.substr(
                  first, content.find_last_not_of(" \n\r\t") - first + 1);
            } else {
              content = "";
            }

            if (content.empty())
              return;

            Discord::DiscordClient &client =
                Discord::DiscordClient::getInstance();
            std::lock_guard<std::recursive_mutex> clientLock(client.getMutex());
            Discord::Message replyMsg;
            replyMsg.id = "pending_" + std::to_string(time(NULL));
            replyMsg.content = content;
            replyMsg.channelId = channelId;
            replyMsg.author = client.getCurrentUser();
            replyMsg.timestamp = TR("message.status.sending");
            replyMsg.type = 19;
            replyMsg.referencedAuthorName = msg.author.global_name.empty()
                                                ? msg.author.username
                                                : msg.author.global_name;

            this->messages.push_back(replyMsg);
            rebuildLayoutCache();
            scrollToBottom();

            client.sendReply(
                channelId, content, msg.id,
                [this,
                 replyMsgId = replyMsg.id](const Discord::Message &sentMsg,
                                           bool success, int errorCode) {
                  std::lock_guard<std::recursive_mutex> lock(messageMutex);
                  for (auto it = this->messages.begin();
                       it != this->messages.end(); ++it) {
                    if (it->id == replyMsgId) {
                      if (success) {
                        *it = sentMsg;
                        Logger::log(
                            "Updated pending reply with confirmed ID: %s",
                            sentMsg.id.c_str());
                      } else {
                        it->timestamp = TR("message.status.failed");
                        Logger::log("Reply failed with code: %d", errorCode);
                      }
                      break;
                    }
                  }
                });
          }
        }
      } else if (action == "Edit") {
        if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
          const auto &msg = messages[selectedIndex];
          SwkbdState swkbd;
          char editbuf[2000];
          strncpy(editbuf, msg.content.c_str(), sizeof(editbuf) - 1);
          editbuf[sizeof(editbuf) - 1] = '\0';
          swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
          swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT |
                                       SWKBD_DARKEN_TOP_SCREEN |
                                       SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET |
                                       SWKBD_ALLOW_POWER | SWKBD_MULTILINE);
          swkbdSetInitialText(&swkbd, editbuf);
          swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, TR("common.cancel").c_str(),
                         false);
          swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, TR("common.save").c_str(),
                         true);
          SwkbdButton button = swkbdInputText(&swkbd, editbuf, sizeof(editbuf));

          if (button == SWKBD_BUTTON_RIGHT) {
            std::string newContent = editbuf;
            size_t first = newContent.find_first_not_of(" \n\r\t");
            if (first != std::string::npos) {
              newContent = newContent.substr(
                  first, newContent.find_last_not_of(" \n\r\t") - first + 1);
            } else {
              newContent = "";
            }

            if (!newContent.empty() && newContent != msg.content) {
              Discord::DiscordClient::getInstance().editMessage(
                  channelId, msg.id, newContent);

              this->messages[selectedIndex].content = newContent;
            }
          }
        }
      } else if (action == "Delete") {
        if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
          std::string mid = this->messages[selectedIndex].id;
          if (Discord::DiscordClient::getInstance().deleteMessage(channelId,
                                                                  mid)) {
            this->messages.erase(this->messages.begin() + selectedIndex);
            if (selectedIndex >= (int)this->messages.size())
              selectedIndex = std::max(0, (int)this->messages.size() - 1);
            rebuildLayoutCache();
          }
        }
      } else if (action == "Retry") {
        if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
          const auto &msg = messages[selectedIndex];
          for (const auto &attach : msg.attachments) {
            std::string url =
                attach.proxy_url.empty() ? attach.url : attach.proxy_url;
            ImageManager::getInstance().clearFailed(url);
            ImageManager::getInstance().prefetch(url, attach.width,
                                                 attach.height);
          }
          for (const auto &sticker : msg.stickers) {
            std::string ext = (sticker.format_type == 4) ? ".gif" : ".png";
            std::string url =
                "https://cdn.discordapp.com/stickers/" + sticker.id + ext;
            ImageManager::getInstance().clearFailed(url);
            ImageManager::getInstance().prefetch(url);
          }
          for (const auto &embed : msg.embeds) {
            if (!embed.image_url.empty()) {
              ImageManager::getInstance().clearFailed(embed.image_url);
              if (!embed.image_proxy_url.empty())
                ImageManager::getInstance().clearFailed(embed.image_proxy_url);

              std::string mainUrl = embed.image_proxy_url.empty()
                                        ? embed.image_url
                                        : embed.image_proxy_url;
              ImageManager::getInstance().prefetch(mainUrl, embed.image_width,
                                                   embed.image_height);
            }
            if (!embed.thumbnail_url.empty()) {
              ImageManager::getInstance().clearFailed(embed.thumbnail_url);
              if (!embed.thumbnail_proxy_url.empty())
                ImageManager::getInstance().clearFailed(
                    embed.thumbnail_proxy_url);

              if (embed.image_url.empty()) {
                std::string mediaUrl = embed.thumbnail_proxy_url.empty()
                                           ? embed.thumbnail_url
                                           : embed.thumbnail_proxy_url;
                ImageManager::getInstance().prefetch(
                    mediaUrl, embed.thumbnail_width, embed.thumbnail_height);
              }

              ImageManager::getInstance().prefetch(embed.thumbnail_url);
            }
          }
        }
      }
    }
    return;
  }

  bool shouldMoveDown = false;
  bool shouldMoveUp = false;

  circlePosition circle;
  hidCircleRead(&circle);
  bool isAnalogMoving = abs(circle.dx) > 35 || abs(circle.dy) > 35;

  if (!isAnalogMoving) {
    if (kDown & KEY_DOWN) {
      shouldMoveDown = true;
      keyRepeatTimer = 0;
    } else if (kHeld & KEY_DOWN) {
      keyRepeatTimer++;
      if (keyRepeatTimer >= REPEAT_INITIAL_DELAY) {
        if ((keyRepeatTimer - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0) {
          shouldMoveDown = true;
        }
      }
    }

    if (kDown & KEY_UP) {
      shouldMoveUp = true;
      keyRepeatTimer = 0;
    } else if (kHeld & KEY_UP) {
      keyRepeatTimer++;
      if (keyRepeatTimer >= REPEAT_INITIAL_DELAY) {
        if ((keyRepeatTimer - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0) {
          shouldMoveUp = true;
        }
      }
    }

    if (!(kHeld & (KEY_UP | KEY_DOWN))) {
      keyRepeatTimer = 0;
    }
  } else {
    keyRepeatTimer = 0;
  }

  bool isManualScrolling = false;

  if (abs(circle.dy) > 35) {
    float scrollDelta = circle.dy * 0.08f;
    targetScrollY -= scrollDelta;

    const float SCREEN_HEIGHT = 230.0f;
    float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
    targetScrollY = std::clamp(targetScrollY, 0.0f, maxScroll);

    isManualScrolling = true;
  } else {
    isManualScrolling = false;
  }

  float scrollSpeed = 0.5f;
  currentScrollY += (targetScrollY - currentScrollY) * scrollSpeed;

  if (showNewMessageIndicator) {
    const float SCREEN_HEIGHT = 230.0f;
    float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
    if (currentScrollY >= maxScroll - 5.0f) {
      showNewMessageIndicator = false;
    }
  }

  if (currentScrollY < 40.0f && !isFetchingHistory && hasMoreHistory &&
      !this->messages.empty()) {
    isFetchingHistory = true;
    fetchOlderMessages();
  }

  if (!isManualScrolling && shouldMoveDown) {
    bool visible = false;
    if (selectedIndex >= 0 && selectedIndex < (int)messagePositions.size()) {
      float y = messagePositions[selectedIndex];
      float h = messageHeights[selectedIndex];
      visible = (y + h > currentScrollY && y < currentScrollY + 230.0f);
    }

    if (!visible && !messagePositions.empty()) {
      auto it = std::lower_bound(messagePositions.begin(),
                                 messagePositions.end(), currentScrollY);
      int snapIdx = std::distance(messagePositions.begin(), it);
      if (snapIdx > 0 &&
          (messagePositions[snapIdx - 1] + messageHeights[snapIdx - 1] >
           currentScrollY)) {
        snapIdx--;
      }
      if (snapIdx >= (int)this->messages.size())
        snapIdx = (int)this->messages.size() - 1;
      selectedIndex = snapIdx;
    } else if (selectedIndex < (int)this->messages.size() - 1) {
      selectedIndex++;
      ensureSelectionVisible();
    }
  } else if (!isManualScrolling && shouldMoveUp) {
    bool visible = false;
    if (selectedIndex >= 0 && selectedIndex < (int)messagePositions.size()) {
      float y = messagePositions[selectedIndex];
      float h = messageHeights[selectedIndex];
      visible = (y + h > currentScrollY && y < currentScrollY + 230.0f);
    }

    if (!visible && !messagePositions.empty()) {
      auto it =
          std::lower_bound(messagePositions.begin(), messagePositions.end(),
                           currentScrollY + 230.0f);
      int snapIdx = std::distance(messagePositions.begin(), it);
      if (snapIdx > 0)
        snapIdx--;
      if (snapIdx >= (int)this->messages.size())
        snapIdx = (int)this->messages.size() - 1;
      selectedIndex = snapIdx;
    } else if (selectedIndex > 0) {
      selectedIndex--;
      ensureSelectionVisible();
    }
  }

  if (kDown & KEY_A) {
    if (isForumView && selectedIndex >= 0 &&
        selectedIndex < (int)this->messages.size()) {
      const auto &msg = this->messages[selectedIndex];
      Discord::DiscordClient::getInstance().setSelectedChannelId(msg.id);
      ScreenManager::getInstance().setScreen(ScreenType::MESSAGES);
      return;
    }
  }

  if (kDown & KEY_Y) {
    openKeyboard();
  }

  if ((kDown & KEY_X) && !(kHeld & KEY_SELECT) && !this->messages.empty()) {
    if (selectedIndex >= 0 && selectedIndex < (int)this->messages.size()) {
      showMessageOptions();
    }
  }

  if (kDown & KEY_B) {
    Discord::DiscordClient::getInstance().setMessageCallback(nullptr);
    Discord::DiscordClient::getInstance().setMessageUpdateCallback(nullptr);
    Discord::DiscordClient::getInstance().setMessageDeleteCallback(nullptr);

    auto &client = Discord::DiscordClient::getInstance();
    Discord::Channel ch = client.getChannel(channelId);
    if (!ch.parent_id.empty()) {
      Discord::Channel parent = client.getChannel(ch.parent_id);
      client.setSelectedChannelId(ch.parent_id);
      if (parent.type == 15) {
        ScreenManager::getInstance().setScreen(ScreenType::FORUM_CHANNEL);
      } else {
        ScreenManager::getInstance().setScreen(ScreenType::MESSAGES);
      }
      return;
    }

    ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
  }
}

float MessageScreen::calculateMessageHeight(const Discord::Message &msg,
                                            bool showHeader) {
  float topMargin = showHeader ? 4.0f : 0.0f;
  float totalH = 0.0f;

  if (isForumView) {
    return 45.0f;
  }

  if (msg.type >= 6 && msg.type <= 10) {
    return topMargin + 26.0f;
  }

  if (msg.type == 19 && !msg.referencedAuthorName.empty()) {
    totalH += 12.0f;
  }

  if (msg.isForwarded) {
    totalH += 15.0f;
  }

  float contentStartTop = totalH;

  if (showHeader) {
    totalH += 14.0f;
  }

  std::string content = msg.content;
  if (!content.empty()) {
    int emojiCount = 0;
    bool isJumbo =
        MessageUtils::isEmojiOnly(content, emojiCount) && emojiCount <= 30;
    float scale = isJumbo ? 1.15f : 0.4f;

    float h = 0;
    float lastLineWidth = 0;
    UI::measureRichText(content, scale, scale, 320.0f, &h, &lastLineWidth);
    totalH += h;

    if (!msg.edited_timestamp.empty()) {
      std::string editedText = TR("message.edited");
      float editedScale = 0.35f;
      float editedWidth = UI::measureText(editedText, editedScale, editedScale);
      float padding = 4.0f;

      if (!isJumbo && (lastLineWidth + padding + editedWidth <= 320.0f)) {
      } else {
        totalH += 10.0f;
      }
    }
  }

  if (!msg.embeds.empty()) {
    float embedMaxWidth = 400.0f - 42.0f - 10.0f;
    for (const auto &embed : msg.embeds) {
      totalH += calculateEmbedHeight(embed, embedMaxWidth) + 6.0f;
    }
  }

  if (!msg.attachments.empty()) {
    for (const auto &attach : msg.attachments) {
      bool isImage = attach.content_type.find("image/") != std::string::npos ||
                     attach.filename.find(".png") != std::string::npos ||
                     attach.filename.find(".jpg") != std::string::npos ||
                     attach.filename.find(".jpeg") != std::string::npos;

      if (isImage) {
        float maxWidth = 348.0f;
        float maxHeight = 260.0f;
        float drawW = maxWidth;
        float drawH = 100.0f;

        if (attach.width > 0 && attach.height > 0) {

          drawW = std::min((float)attach.width, maxWidth);
          if (attach.width > 160)
            drawW = maxWidth;

          float aspect = (float)attach.width / attach.height;
          drawH = drawW / aspect;

          if (drawH > maxHeight) {
            drawH = maxHeight;
            drawW = drawH * aspect;
          }
        }
        totalH += drawH + 4.0f;
      } else {
        totalH += 12.0f;
      }
    }
  }

  if (!msg.stickers.empty()) {
    for (const auto &sticker : msg.stickers) {
      if (sticker.format_type == 3) {

        totalH += 12.0f;
      } else {
        totalH += 100.0f + 4.0f;
      }
    }
  }

  if (!msg.reactions.empty()) {
    float textOffsetX = 42.0f;
    float reactionX = textOffsetX;
    float rowHeight = 21.0f;
    float gap = 4.0f;
    float wrapBound = 320.0f;
    float currentReactionsH = rowHeight;

    for (const auto &react : msg.reactions) {
      std::string countStr = std::to_string(react.count);
      float countW = UI::measureText(countStr, 0.4f, 0.4f);
      float emojiW = 18.0f;
      float boxPad = 6.0f;
      float boxW = emojiW + countW + boxPad + 4.0f;

      if (reactionX + boxW > textOffsetX + wrapBound) {
        reactionX = textOffsetX;
        currentReactionsH += rowHeight + gap;
      }
      reactionX += boxW + gap;
    }
    totalH += currentReactionsH + 5.0f;
  }

  if (showHeader) {
    if (totalH < contentStartTop + 28.0f) {
      totalH = contentStartTop + 28.0f;
    }
  }

  return topMargin + totalH + 2.0f;
}

float MessageScreen::calculateMessageHeight(const Discord::Message &msg) {
  return calculateMessageHeight(msg, true);
}

float MessageScreen::drawForumMessage(const Discord::Message &msg, float y,
                                      bool isSelected) {
  float drawY = y + 2.0f;
  float cardH = 40.0f;

  u32 bgColor = isSelected ? ScreenManager::colorBackgroundLight()
                           : ScreenManager::colorBackgroundDark();
  if (isSelected) {
    C2D_DrawRectSolid(0.0f, drawY, 0.5f, 400.0f, cardH, bgColor);
    C2D_DrawRectSolid(0.0f, drawY, 0.5f, 4.0f, cardH,
                      ScreenManager::colorPrimary());
    C2D_DrawRectSolid(10.0f, drawY + cardH - 1.0f, 0.5f, 380.0f, 1.0f,
                      C2D_Color32(60, 60, 70, 255));
  }

  std::string icon = "#";
  if (msg.type == 10)
    icon = "!";
  else if (msg.type == 12)
    icon = "@";

  drawText(15.0f, drawY + 10.0f, 0.5f, 0.6f, 0.6f,
           ScreenManager::colorTextMuted(), icon);

  std::string name =
      getTruncatedText(msg.content, 400.0f - 40.0f - 15.0f, 0.5f, 0.5f);
  drawText(40.0f, drawY + 8.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorText(),
           name);

  return 45.0f;
}

float MessageScreen::drawSystemMessage(const Discord::Message &msg, float y,
                                       float topMargin, float height) {
  float blockHeight = 18.0f;
  float drawY =
      y + topMargin + ((height - topMargin - blockHeight) / 2.0f) + 1.0f;

  u32 iconColor = ScreenManager::colorSuccess();
  std::string icon = "->";
  std::string text = "";
  std::string authorName = msg.author.global_name.empty()
                               ? msg.author.username
                               : msg.author.global_name;

  u32 nameColor = ScreenManager::colorText();

  if (msg.type == 7) {
    text = TR("message.system.joined");
    iconColor = C2D_Color32(55, 151, 93, 255);
  } else if (msg.type == 8 || msg.type == 9 || msg.type == 10) {
    text = TR("message.system.boosted");
    iconColor = C2D_Color32(253, 112, 243, 255);
  } else if (msg.type == 6) {
    iconColor = ScreenManager::colorTextMuted();
    text = TR("message.system.pinned");
  }

  if (msg.type == 6 || msg.type == 7 || (msg.type >= 8 && msg.type <= 10)) {
    std::string iconPath;
    if (msg.type == 6)
      iconPath = "romfs:/discord-icons/pin.png";
    else if (msg.type == 7)
      iconPath = "romfs:/discord-icons/arrow-right.png";
    else
      iconPath = "romfs:/discord-icons/boostgem.png";

    C3D_Tex *tex = ImageManager::getInstance().getLocalImage(iconPath, true);
    if (tex) {
      float iconSize = 14.0f;
      Tex3DS_SubTexture subtex = {
          (u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
      C2D_Image img = {tex, &subtex};
      C2D_ImageTint tint;
      C2D_PlainImageTint(&tint, iconColor, 1.0f);

      C2D_DrawImageAt(img, 17.0f, drawY, 0.5f, &tint, iconSize / tex->width,
                      iconSize / tex->height);
    } else {
      drawText(12.0f, drawY, 0.55f, 0.35f, 0.35f, iconColor, icon);
    }
  } else {
    drawText(12.0f, drawY, 0.55f, 0.35f, 0.35f, iconColor, icon);
  }

  const float textOffsetX = 42.0f;
  float nameW = UI::measureRichText(authorName, 0.42f, 0.42f);
  drawRichText(textOffsetX, drawY, 0.5f, 0.42f, 0.42f, nameColor, authorName);

  drawRichText(textOffsetX + nameW, drawY, 0.5f, 0.42f, 0.42f,
               ScreenManager::colorTextMuted(), text);
  return height;
}

float MessageScreen::drawReplyPreview(const Discord::Message &msg, float x,
                                      float y) {
  if (msg.type != 19 || msg.referencedAuthorName.empty()) {
    return y;
  }

  std::string arrowPath = "romfs:/discord-icons/curve.png";
  auto arrowInfo = ImageManager::getInstance().getImageInfo(arrowPath);
  if (!arrowInfo.tex || arrowInfo.failed) {
    ImageManager::getInstance().getLocalImage(arrowPath, true);
    arrowInfo = ImageManager::getInstance().getImageInfo(arrowPath);
  }

  std::string author = !msg.referencedAuthorNickname.empty()
                           ? msg.referencedAuthorNickname
                           : msg.referencedAuthorName;
  std::string colon = ": ";

  float prefixW = 12.0f;
  float authorW = UI::measureRichText(author, 0.35f, 0.35f);
  float colonW = UI::measureRichText(colon, 0.35f, 0.35f);

  float maxWidthRef = 310.0f - x - (prefixW + authorW + colonW);

  std::string cleanedContent = msg.referencedContent;
  std::replace(cleanedContent.begin(), cleanedContent.end(), '\n', ' ');
  std::replace(cleanedContent.begin(), cleanedContent.end(), '\r', ' ');

  auto lines = MessageUtils::wrapText(cleanedContent, maxWidthRef, 0.35f);
  std::string replyContent = "";
  if (!lines.empty()) {
    replyContent = lines[0];
    if (lines.size() > 1)
      replyContent += "...";
  }

  float curX = x;

  if (arrowInfo.tex) {
    float iconSize = 8.0f;
    float uMax = (float)arrowInfo.originalW / arrowInfo.tex->width;
    float vMax = (float)arrowInfo.originalH / arrowInfo.tex->height;
    Tex3DS_SubTexture subtex = {(u16)arrowInfo.originalW,
                                (u16)arrowInfo.originalH,
                                0.0f,
                                1.0f,
                                uMax,
                                1.0f - vMax};
    C2D_Image img = {arrowInfo.tex, &subtex};
    C2D_DrawImageAt(img, curX + 1.0f, y + 2.0f, 0.5f, nullptr,
                    iconSize / arrowInfo.originalW,
                    iconSize / arrowInfo.originalH);
  } else {
    drawText(curX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
             "↳ ");
  }
  curX += prefixW;

  u32 authorColor = ScreenManager::colorTextMuted();
  if (msg.referencedAuthorColor != 0) {
    authorColor = C2D_Color32((msg.referencedAuthorColor >> 16) & 0xFF,
                              (msg.referencedAuthorColor >> 8) & 0xFF,
                              msg.referencedAuthorColor & 0xFF, 255);
  }

  drawRichText(curX, y, 0.5f, 0.35f, 0.35f, authorColor, author);
  curX += authorW;
  drawRichText(curX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
               colon);
  curX += colonW;

  drawRichText(curX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
               replyContent);
  return y + 12.0f;
}

float MessageScreen::drawForwardHeader(const Discord::Message &msg, float x,
                                       float y) {
  if (!msg.isForwarded)
    return y;

  std::string iconPath = "romfs:/discord-icons/arrow-angle-right-up.png";
  C3D_Tex *icon = UI::ImageManager::getInstance().getLocalImage(iconPath, true);
  if (icon) {
    float iconSize = 10.0f;
    Tex3DS_SubTexture subtex = {
        (u16)icon->width, (u16)icon->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {icon, &subtex};

    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, ScreenManager::colorTextMuted(), 1.0f);

    C2D_DrawImageAt(img, x + 2.0f, y + 2.0f, 0.5f, &tint,
                    iconSize / icon->width, iconSize / icon->height);

    drawText(x + iconSize + 6.0f, y, 0.5f, 0.38f, 0.38f,
             ScreenManager::colorTextMuted(), TR("message.forwarded"));
  } else {
    drawText(x + 2.0f, y, 0.5f, 0.38f, 0.38f, ScreenManager::colorTextMuted(),
             "-> " + TR("message.forwarded"));
  }

  return y + 15.0f;
}

float MessageScreen::drawAuthorHeader(const Discord::Message &msg, float x,
                                      float y, bool showHeader) {
  if (!showHeader)
    return y;

  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

  std::string displayName;
  if (!msg.member.nickname.empty()) {
    displayName = msg.member.nickname;
  } else {
    displayName =
        client.getMemberDisplayName(guildId, msg.author.id, msg.author);
  }

  u32 nameColor = ScreenManager::colorText();
  int roleColor = 0;

  if (!msg.member.role_ids.empty()) {
    roleColor = client.getRoleColor(guildId, msg.member);
  }

  if (roleColor == 0) {
    roleColor = client.getRoleColor(guildId, msg.author.id);
    if (roleColor == 0 && !guildId.empty()) {
      Discord::Member cached = client.getMember(guildId, msg.author.id);
      if (cached.user_id.empty()) {
        uint64_t now = osGetTime();
        auto it = failedMemberFetches.find(msg.author.id);
        bool onCooldown = (it != failedMemberFetches.end() && now < it->second);

        if (!onCooldown && pendingMemberFetches.find(msg.author.id) ==
                               pendingMemberFetches.end()) {
          pendingMemberFetches.insert(msg.author.id);
          std::string uid = msg.author.id;
          client.fetchMember(
              guildId, uid,
              [this, uid, token = aliveToken](const Discord::Member &m) {
                if (!*token)
                  return;
                if (m.user_id.empty()) {
                  this->failedMemberFetches[uid] = osGetTime() + (30 * 1000);
                }
                this->pendingMemberFetches.erase(uid);
              });
        }
      }
    }
  }

  if (roleColor != 0) {
    int r = (roleColor >> 16) & 0xFF;
    int g = (roleColor >> 8) & 0xFF;
    int b = roleColor & 0xFF;
    nameColor = C2D_Color32(r, g, b, 255);
  }

  float avatarX = 10.0f;
  float avatarSize = 28.0f;

  C3D_Tex *avatarTex = Discord::AvatarCache::getInstance().getAvatar(
      msg.author.id, msg.author.avatar, msg.author.discriminator);
  if (avatarTex) {
    Tex3DS_SubTexture subtex = {
        (u16)avatarTex->width, (u16)avatarTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {avatarTex, &subtex};
    C2D_DrawImageAt(img, avatarX, y, 0.5f, nullptr,
                    avatarSize / avatarTex->width,
                    avatarSize / avatarTex->height);
  } else {
    Discord::AvatarCache::getInstance().prefetchAvatar(
        msg.author.id, msg.author.avatar, msg.author.discriminator);

    C2D_DrawRectSolid(avatarX, y, 0.5f, avatarSize, avatarSize,
                      C2D_Color32(80, 80, 100, 255));
    std::string initial = displayName.empty() ? "?" : displayName.substr(0, 1);
    drawText(avatarX + 10, y + 8, 0.6f, 0.45f, 0.45f,
             C2D_Color32(255, 255, 255, 255), initial);
  }

  drawRichText(x, y, 0.5f, 0.45f, 0.45f, nameColor, displayName);
  float nameWidth = UI::measureRichText(displayName, 0.45f, 0.45f);
  float timeX = x + nameWidth + 8.0f;
  std::string time = MessageUtils::formatTimestamp(msg.timestamp);

  drawText(timeX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), time);
  return y + 14.0f;
}

float MessageScreen::drawMessageContent(const Discord::Message &msg, float x,
                                        float y) {
  std::string content = msg.content;
  if (content.empty())
    return y;

  int emojiCount = 0;
  bool isJumbo =
      MessageUtils::isEmojiOnly(content, emojiCount) && emojiCount <= 30;
  float scale = isJumbo ? 1.15f : 0.4f;

  float h = 0;
  float lastLineWidth = 0;
  UI::measureRichText(content, scale, scale, 320.0f, &h, &lastLineWidth);
  UI::drawRichText(x, y, 0.5f, scale, scale, ScreenManager::colorText(),
                   content, 320.0f);
  float newY = y + h;

  if (!msg.edited_timestamp.empty()) {
    std::string editedText = TR("message.edited");
    float editedScale = 0.35f;
    float editedWidth = UI::measureText(editedText, editedScale, editedScale);
    float padding = 4.0f;
    float currentLineHeight = scale * 30.0f;

    if (!isJumbo && (lastLineWidth + padding + editedWidth <= 320.0f)) {
      drawText(x + lastLineWidth + padding, newY - currentLineHeight + 1.0f,
               0.5f, editedScale, editedScale, ScreenManager::colorTextMuted(),
               editedText);
    } else {
      drawText(x, newY, 0.5f, editedScale, editedScale,
               ScreenManager::colorTextMuted(), editedText);
      newY += 10.0f;
    }
  }
  return newY;
}

float MessageScreen::drawAttachments(const Discord::Message &msg, float x,
                                     float y) {
  if (msg.attachments.empty())
    return y;

  float newY = y;
  for (const auto &attach : msg.attachments) {
    if (attach.content_type.find("image/") != std::string::npos ||
        attach.filename.find(".png") != std::string::npos ||
        attach.filename.find(".jpg") != std::string::npos ||
        attach.filename.find(".jpeg") != std::string::npos) {

      std::string imageUrl =
          attach.proxy_url.empty() ? attach.url : attach.proxy_url;
      auto info = ImageManager::getInstance().getImageInfo(imageUrl);

      float maxWidth = 348.0f;
      float maxHeight = 260.0f;
      float drawW = maxWidth;
      float drawH = 100.0f;

      bool hasMeta = (attach.width > 0 && attach.height > 0);
      if (hasMeta) {
        float aspect = (float)attach.width / attach.height;
        drawW = std::min((float)attach.width, maxWidth);
        if (attach.width > 160)
          drawW = maxWidth;

        drawH = drawW / aspect;
        if (drawH > maxHeight) {
          drawH = maxHeight;
          drawW = drawH * aspect;
        }
      }

      if (info.tex) {
        float aspect = (float)info.originalW / info.originalH;
        drawW = std::min((float)info.originalW, 348.0f);
        if (info.originalW > 160)
          drawW = 348.0f;

        drawH = drawW / aspect;
        if (drawH > maxHeight) {
          drawH = maxHeight;
          drawW = drawH * aspect;
        }

        float uMax = (float)info.originalW / info.tex->width;
        float vMax = (float)info.originalH / info.tex->height;
        Tex3DS_SubTexture subtex = {
            (u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax,
            1.0f - vMax};

        const C2D_Image img = {info.tex, &subtex};
        C2D_DrawImageAt(img, x, newY, 0.5f, nullptr, drawW / info.originalW,
                        drawH / info.originalH);
        newY += drawH + 4.0f;
      } else if (info.failed) {
        u32 errorBg = (Config::getInstance().getThemeType() == 1)
                          ? C2D_Color32(255, 235, 235, 255)
                          : C2D_Color32(60, 40, 40, 255);
        C2D_DrawRectSolid(x, newY, 0.5f, drawW, drawH, errorBg);
        drawText(x + 5, newY + (drawH / 2) - 6, 0.5f, 0.3f, 0.3f,
                 ScreenManager::colorError(), TR("message.image_failed"));
        newY += drawH + 4.0f;
      } else {
        ImageManager::getInstance().prefetch(
            imageUrl, attach.width, attach.height,
            Network::RequestPriority::INTERACTIVE);

        u32 placeholderBg = (Config::getInstance().getThemeType() == 1)
                                ? ScreenManager::colorBackgroundDark()
                                : ScreenManager::colorBackgroundDark();
        C2D_DrawRectSolid(x, newY, 0.5f, drawW, drawH, placeholderBg);
        drawText(x + 5, newY + (drawH / 2) - 6, 0.5f, 0.3f, 0.3f,
                 ScreenManager::colorTextMuted(), TR("common.loading"));
        newY += drawH + 4.0f;
      }
    } else {
      std::string fileInfo =
          Core::I18n::format(TR("message.file"), attach.filename);
      drawText(x, newY, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
               fileInfo);
      newY += 12.0f;
    }
  }
  return newY;
}

float MessageScreen::drawStickers(const Discord::Message &msg, float x,
                                  float y) {
  if (msg.stickers.empty())
    return y;

  float newY = y;
  for (const auto &sticker : msg.stickers) {
    if (sticker.format_type == 3) {
      std::string label =
          Core::I18n::format(TR("message.sticker"), sticker.name);
      drawText(x, newY, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
               label);
      newY += 12.0f;
      continue;
    }

    std::string ext = ".png";
    if (sticker.format_type == 4)
      ext = ".gif";

    std::string stickerUrl =
        "https://cdn.discordapp.com/stickers/" + sticker.id + ext;
    float stickerSize = 100.0f;

    UI::ImageManager::ImageInfo info =
        UI::ImageManager::getInstance().getImageInfo(stickerUrl);
    if (info.tex) {
      float uMax = (float)info.originalW / info.tex->width;
      float vMax = (float)info.originalH / info.tex->height;
      Tex3DS_SubTexture subtex = {
          (u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax,
          1.0f - vMax};
      const C2D_Image img = {info.tex, &subtex};
      C2D_DrawImageAt(img, x, newY, 0.5f, nullptr, stickerSize / info.originalW,
                      stickerSize / info.originalH);
      newY += stickerSize + 4.0f;
    } else if (info.failed) {
      u32 stickerBg = (Config::getInstance().getThemeType() == 1)
                          ? C2D_Color32(255, 235, 235, 255)
                          : C2D_Color32(60, 40, 40, 255);
      C2D_DrawRectSolid(x, newY, 0.5f, stickerSize, stickerSize, stickerBg);
      drawText(x + 5, newY + (stickerSize / 2) - 6, 0.55f, 0.3f, 0.3f,
               ScreenManager::colorError(), TR("message.sticker_failed"));
      newY += stickerSize + 4.0f;
    } else {
      UI::ImageManager::getInstance().prefetch(
          stickerUrl, 160, 160, Network::RequestPriority::INTERACTIVE);
      u32 stickerBg = (Config::getInstance().getThemeType() == 1)
                          ? ScreenManager::colorBackgroundDark()
                          : ScreenManager::colorBackgroundDark();
      C2D_DrawRectSolid(x, newY, 0.5f, stickerSize, stickerSize, stickerBg);
      drawText(x + 5, newY + (stickerSize / 2) - 6, 0.55f, 0.3f, 0.3f,
               ScreenManager::colorTextMuted(), TR("common.loading"));
      newY += stickerSize + 4.0f;
    }
  }
  return newY;
}

float MessageScreen::drawReactions(const Discord::Message &msg, float x,
                                   float y, bool isSelected) {
  if (msg.reactions.empty())
    return y;

  float reactionX = x;
  float rowHeight = 21.0f;
  float gap = 4.0f;
  float newY = y;

  newY += 3.0f;

  struct ReactionDrawInfo {
    float x, y, boxW;
    const Discord::Reaction *react;
  };
  std::vector<ReactionDrawInfo> drawInfos;

  for (const auto &react : msg.reactions) {
    std::string countStr = std::to_string(react.count);
    float countW = UI::measureText(countStr, 0.4f, 0.4f);
    float emojiW = 18.0f;
    float boxPad = 6.0f;
    float boxW = emojiW + countW + boxPad + 4.0f;

    if (reactionX + boxW > x + 320.0f) {
      reactionX = x;
      newY += rowHeight + gap;
    }

    u32 boxBg = react.me ? ScreenManager::colorReactionMe()
                         : ScreenManager::colorReaction();

    if (isSelected) {
      u8 r = (boxBg >> 0) & 0xFF;
      u8 g = (boxBg >> 8) & 0xFF;
      u8 b = (boxBg >> 16) & 0xFF;
      boxBg = C2D_Color32(std::min(r + 20, 255), std::min(g + 20, 255),
                          std::min(b + 20, 255), 255);
    }

    if (react.me) {
      u32 borderCol = ScreenManager::colorSelection();
      drawRoundedRect(reactionX, newY, 0.45f, boxW, rowHeight, 6.0f, borderCol);
      drawRoundedRect(reactionX + 1.0f, newY + 1.0f, 0.46f, boxW - 2.0f,
                      rowHeight - 2.0f, 5.0f, boxBg);
    } else {
      drawRoundedRect(reactionX, newY, 0.45f, boxW, rowHeight, 6.0f, boxBg);
    }

    drawInfos.push_back({reactionX, newY, boxW, &react});

    reactionX += boxW + gap;
  }

  for (const auto &info : drawInfos) {
    float emojiX = info.x + 4.0f;
    float emojiY = info.y + 2.0f;
    float emojiW = 18.0f;

    if (!info.react->emoji.id.empty()) {
      EmojiManager::EmojiInfo emojiInfo =
          UI::EmojiManager::getInstance().getEmojiInfo(info.react->emoji.id);
      if (emojiInfo.tex) {
        float uMax = (float)emojiInfo.originalW / emojiInfo.tex->width;
        float vMax = (float)emojiInfo.originalH / emojiInfo.tex->height;
        Tex3DS_SubTexture subtex = {(u16)emojiInfo.originalW,
                                    (u16)emojiInfo.originalH,
                                    0.0f,
                                    1.0f,
                                    uMax,
                                    1.0f - vMax};
        float scale =
            std::min(16.0f / emojiInfo.originalW, 16.0f / emojiInfo.originalH);
        float drawEmojiX =
            emojiX + (16.0f - emojiInfo.originalW * scale) / 2.0f;
        float drawEmojiY =
            emojiY + (16.0f - emojiInfo.originalH * scale) / 2.0f;
        C2D_Image img = {emojiInfo.tex, &subtex};
        C2D_DrawImageAt(img, drawEmojiX, drawEmojiY, 0.47f, nullptr, scale,
                        scale);
      } else {
        UI::EmojiManager::getInstance().prefetchEmoji(info.react->emoji.id);
        drawText(emojiX, emojiY + 2.0f, 0.47f, 0.4f, 0.4f,
                 ScreenManager::colorTextMuted(), "?");
      }
    } else {
      std::string hex = MessageUtils::getEmojiFilename(info.react->emoji.name);
      EmojiManager::EmojiInfo emojiInfo =
          UI::EmojiManager::getInstance().getTwemojiInfo(hex);

      if (emojiInfo.tex) {
        float uMax = (float)emojiInfo.originalW / emojiInfo.tex->width;
        float vMax = (float)emojiInfo.originalH / emojiInfo.tex->height;
        Tex3DS_SubTexture subtex = {(u16)emojiInfo.originalW,
                                    (u16)emojiInfo.originalH,
                                    0.0f,
                                    1.0f,
                                    uMax,
                                    1.0f - vMax};
        float scale =
            std::min(16.0f / emojiInfo.originalW, 16.0f / emojiInfo.originalH);
        float drawEmojiX =
            emojiX + (16.0f - emojiInfo.originalW * scale) / 2.0f;
        float drawEmojiY =
            emojiY + (16.0f - emojiInfo.originalH * scale) / 2.0f;
        C2D_Image img = {emojiInfo.tex, &subtex};
        C2D_DrawImageAt(img, drawEmojiX, drawEmojiY, 0.47f, nullptr, scale,
                        scale);
      } else {
        drawText(emojiX, emojiY + 2.0f, 0.47f, 0.5f, 0.5f,
                 ScreenManager::colorText(), info.react->emoji.name);
      }
    }

    std::string countStr = std::to_string(info.react->count);
    drawText(info.x + emojiW + 6.0f, info.y + 5.0f, 0.47f, 0.4f, 0.4f,
             info.react->me ? ScreenManager::colorText()
                            : ScreenManager::colorTextMuted(),
             countStr);
  }

  return newY + rowHeight + 4.0f;
}

float MessageScreen::drawMessage(const Discord::Message &msg, float y,
                                 float maxWidth, bool isSelected,
                                 bool showHeader) {
  float height = calculateMessageHeight(msg, showHeader);
  float topMargin = showHeader ? 4.0f : 0.0f;
  const float textOffsetX = 42.0f;

  if (isForumView) {
    return drawForumMessage(msg, y, isSelected);
  }

  if (isSelected) {
    float highlightY = y + topMargin;
    float highlightH = height - topMargin;
    drawRoundedRect(4.0f, highlightY, 0.1f, 392.0f, highlightH, 6.0f,
                    ScreenManager::colorBackgroundLight());
  }

  if (msg.type >= 6 && msg.type <= 10) {
    return drawSystemMessage(msg, y, topMargin, height);
  }

  if (msg.isForwarded) {
    float barX = 38.0f;
    float barY = y + topMargin + 1.0f;
    float barW = 2.0f;
    float barH = height - topMargin - 2.0f;
    C2D_DrawRectSolid(barX, barY, 0.45f, barW, barH,
                      ScreenManager::colorTextMuted());
  }

  float contentY = y + topMargin + 1.0f;

  contentY = drawReplyPreview(msg, textOffsetX, contentY);
  contentY = drawForwardHeader(msg, textOffsetX, contentY);

  float avatarTopY = contentY;
  contentY = drawAuthorHeader(msg, textOffsetX, contentY, showHeader);

  if (!showHeader && isSelected) {
    std::string time = MessageUtils::formatTimeOnly(msg.timestamp);
    drawText(10.0f, contentY, 0.5f, 0.35f, 0.35f,
             ScreenManager::colorTextMuted(), time);
  }

  contentY = drawMessageContent(msg, textOffsetX, contentY);

  if (!msg.embeds.empty()) {
    for (const auto &embed : msg.embeds) {
      contentY += renderEmbed(embed, textOffsetX, contentY,
                              400.0f - textOffsetX - 10.0f);
      contentY += 6.0f;
    }
  }

  contentY = drawAttachments(msg, textOffsetX, contentY);
  contentY = drawStickers(msg, textOffsetX, contentY);
  contentY = drawReactions(msg, textOffsetX, contentY, isSelected);

  if (showHeader) {
    if (contentY < avatarTopY + 28.0f)
      contentY = avatarTopY + 28.0f;
  }

  return height;
}

void MessageScreen::renderTop(C3D_RenderTarget *target) {
  C2D_TargetClear(target, ScreenManager::colorBackground());
  C2D_SceneBegin(target);

  const float SCREEN_HEIGHT = 230.0f;

  if (isLoading) {
    drawCenteredRichText(
        110.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
        Core::I18n::getInstance().get("common.loading"), 400.0f);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(messageMutex);

  if (this->messages.empty() && !isLoading) {
    drawCenteredRichText(
        110.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
        Core::I18n::getInstance().get("message.no_messages"), 400.0f);
    return;
  }

  float availableHeight = 230.0f;
  float topPadding = 10.0f;
  if (isFetchingHistory) {
    drawCenteredRichText(
        5.0f, 0.55f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
        Core::I18n::getInstance().get("message.loading_history"), 400.0f);
    topPadding += 15.0f;
    availableHeight -= 15.0f;
  }

  float yOffset = std::max(0.0f, SCREEN_HEIGHT - totalContentHeight);

  float yStart = -currentScrollY + yOffset;
  const float MARGIN = 10.0f;
  const float TOP_MARGIN = 30.0f;

  for (size_t i = 0; i < messages.size(); i++) {
    if (i >= messagePositions.size() || i >= messageHeights.size())
      break;

    float msgY = yStart + messagePositions[i];
    float msgH = messageHeights[i];

    if (msgY + msgH < -TOP_MARGIN || msgY > SCREEN_HEIGHT + MARGIN)
      continue;

    bool showDateSeparator = false;
    std::string currDate = "";

    if (i == 0) {
      showDateSeparator = true;
      currDate = MessageUtils::getLocalDateString(this->messages[i].timestamp);
    } else if (this->messages[i].timestamp != "Sending...") {
      currDate = MessageUtils::getLocalDateString(this->messages[i].timestamp);
      std::string prevDate =
          MessageUtils::getLocalDateString(this->messages[i - 1].timestamp);
      if (currDate != prevDate) {
        showDateSeparator = true;
      }
    }

    float dateY = msgY - 20.0f;

    float renderTopY = showDateSeparator ? dateY : msgY;
    float renderBottomY = msgY + msgH;

    const float MARGIN = 10.0f;

    const float TOP_MARGIN = showDateSeparator ? 30.0f : MARGIN;

    if (renderBottomY < -TOP_MARGIN || renderTopY > SCREEN_HEIGHT + MARGIN)
      continue;

    if (showDateSeparator) {
      if (dateY > -30.0f && dateY < SCREEN_HEIGHT) {
        float lineY = dateY + 7.0f;
        u32 lineColor = C2D_Color32(80, 80, 85, 255);
        C2D_DrawRectSolid(10.0f, lineY, 0.7f, 130.0f, 1.0f, lineColor);
        C2D_DrawRectSolid(260.0f, lineY, 0.7f, 130.0f, 1.0f, lineColor);

        drawCenteredRichText(dateY, 0.7f, 0.4f, 0.4f,
                             ScreenManager::colorTextMuted(), currDate, 400.0f);
      }
    }

    bool isSelected = (i == (size_t)selectedIndex);
    bool showHeader = (i == 0) || !MessageUtils::canGroupWithPrevious(
                                      messages[i], messages[i - 1]);

    if (showDateSeparator)
      showHeader = true;

    drawMessage(this->messages[i], msgY, 400.0f, isSelected, showHeader);
  }

  if (showNewMessageIndicator) {
    float indicatorY = 205.0f;
    float indicatorW = 130.0f;
    float indicatorH = 22.0f;
    float indicatorX = (400.0f - indicatorW) / 2.0f;

    drawRoundedRect(indicatorX, indicatorY, 0.65f, indicatorW, indicatorH,
                    11.0f, ScreenManager::colorSelection());
    std::string text = TR("message.new_indicator");
    if (newMessageCount > 0) {
      text = Core::I18n::format(TR("message.new_indicator_count"),
                                std::to_string(newMessageCount));
    }
    drawCenteredRichText(indicatorY + 5.0f, 0.66f, 0.4f, 0.4f,
                         ScreenManager::colorWhite(), text, 400.0f);
  }

  if (isMenuOpen) {
    renderMenu();
  }
}

void MessageScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_DrawRectSolid(0, 0, 0.0f, 320, 240, ScreenManager::colorBackgroundDark());

  float headerX = 35.0f;

  std::string iconPath;
  if (channelType == 5) {
    iconPath = "romfs:/discord-icons/announcement.png";
  } else if (channelType == 10 || channelType == 11 || channelType == 12 ||
             channelType == 1 || channelType == 3) {
    iconPath = "romfs:/discord-icons/chat.png";
  } else {
    iconPath = "romfs:/discord-icons/text.png";
  }

  C3D_Tex *icon = UI::ImageManager::getInstance().getLocalImage(iconPath);
  if (icon) {
    float iconSize = 16.0f;
    Tex3DS_SubTexture subtex = {
        (u16)icon->width, (u16)icon->height, 0.0f, 1.0f, 1.0f, 0.0f};
    C2D_Image img = {icon, &subtex};

    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);

    C2D_DrawImageAt(img, 35.0f, 10.0f, 0.51f, &tint, iconSize / icon->width,
                    iconSize / icon->height);
    headerX = 35.0f + iconSize + 5.0f;
  } else {
    drawText(35.0f, 10.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(),
             "#");
    headerX = 50.0f;
  }

  drawRichText(headerX, 10.0f, 0.5f, 0.55f, 0.55f, ScreenManager::colorText(),
               truncatedChannelName);

  C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());

  std::string displayTopic =
      channelTopic.empty() ? Core::I18n::getInstance().get("common.no_topic")
                           : channelTopic;

  float topicY = 40.0f;

  drawText(10.0f, topicY, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(),
           Core::I18n::getInstance().get("message.topic"));
  topicY += 15.0f;

  auto lines = MessageUtils::wrapText(displayTopic, 300.0f, 0.4f);
  int lineCount = 0;

  for (const auto &line : lines) {
    if (lineCount >= 10)
      break;

    drawRichText(10.0f, topicY, 0.5f, 0.4f, 0.4f, ScreenManager::colorText(),
                 line);
    topicY += 13.0f;
    lineCount++;
  }

  bool canSend =
      Discord::DiscordClient::getInstance().canSendMessage(channelId);

  std::string hints = "\uE079\uE07A: " + TR("common.navigate") + "  ";
  if (isMenuOpen) {
    hints +=
        "\uE000: " + TR("common.select") + "  \uE001: " + TR("common.close");
  } else if (isForumView) {
    hints += "\uE000: " + TR("common.open") + "  \uE001: " + TR("common.back");
  } else {
    if (canSend) {
      hints += "\uE003: " + TR("common.type") + "  ";
    }
    hints += "\uE002: " + TR("common.menu") + "  \uE001: " + TR("common.back");
  }

  drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 25.0f, 0.5f, 0.4f, 0.4f,
           ScreenManager::colorTextMuted(), hints);

  auto typingUsers =
      Discord::DiscordClient::getInstance().getTypingUsers(channelId);
  if (!typingUsers.empty()) {
    std::string typingText = "";
    if (typingUsers.size() == 1) {
      typingText = typingUsers[0].displayName + " " + TR("common.is_typing");
    } else if (typingUsers.size() <= 3) {
      for (size_t i = 0; i < typingUsers.size(); i++) {
        typingText += typingUsers[i].displayName;
        if (i < typingUsers.size() - 1)
          typingText += ", ";
      }
      typingText += " " + TR("common.are_typing");
    } else {
      typingText = TR("common.several_users_typing");
    }

    drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 50.0f, 0.5f, 0.4f, 0.4f,
             ScreenManager::colorSelection(), typingText);
  }

  const float SCREEN_HEIGHT = 230.0f;
  float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);

  if (targetScrollY < maxScroll - 10.0f) {
    float btnW = 30.0f;
    float btnH = 30.0f;
    float btnX = 320.0f - btnW - 10.0f;
    float btnY = 240.0f - btnH - 10.0f;

    drawRoundedRect(btnX, btnY, 0.5f, btnW, btnH, 6.0f,
                    ScreenManager::colorBackgroundLight());

    u32 borderCol = ScreenManager::colorSelection();
    drawRoundedRect(btnX, btnY, 0.51f, btnW, 1.0f, 0.5f, borderCol);
    drawRoundedRect(btnX, btnY + btnH - 1.0f, 0.51f, btnW, 1.0f, 0.5f,
                    borderCol);
    drawRoundedRect(btnX, btnY, 0.51f, 1.0f, btnH, 0.5f, borderCol);
    drawRoundedRect(btnX + btnW - 1.0f, btnY, 0.51f, 1.0f, btnH, 0.5f,
                    borderCol);

    float centerX = btnX + (btnW / 2.0f);
    float centerY = btnY + (btnH / 2.0f) - 2.5f;
    float triSize = 6.0f;

    C2D_DrawTriangle(
        centerX - triSize, centerY - (triSize / 2), ScreenManager::colorText(),
        centerX + triSize, centerY - (triSize / 2), ScreenManager::colorText(),
        centerX, centerY + triSize, ScreenManager::colorText(), 0.55f);

    drawRoundedRect(centerX - triSize, centerY + triSize + 1.0f, 0.55f,
                    triSize * 2, 1.5f, 0.75f, ScreenManager::colorText());
  }
}

void MessageScreen::fetchOlderMessages() {
  if (this->messages.empty()) {
    isFetchingHistory = false;
    return;
  }

  std::string beforeId = this->messages.front().id;
  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

  client.fetchMessagesBeforeAsync(
      channelId, beforeId, 25,
      [this,
       token = aliveToken](const std::vector<Discord::Message> &olderMessages) {
        if (!*token)
          return;
        if (!olderMessages.empty()) {
          std::vector<Discord::Message> reversed = olderMessages;
          std::reverse(reversed.begin(), reversed.end());

          float oldTotalHeight = totalContentHeight;
          size_t addedCount = 0;

          {
            std::lock_guard<std::recursive_mutex> lock(messageMutex);
            this->messages.insert(this->messages.begin(), reversed.begin(),
                                  reversed.end());
            selectedIndex += reversed.size();
            addedCount = reversed.size();
          }

          rebuildLayoutCache();

          float heightDiff = totalContentHeight - oldTotalHeight;
          currentScrollY += heightDiff;
          targetScrollY += heightDiff;

          Logger::log("Loaded %d older messages async, adjusted scroll by %.2f",
                      addedCount, heightDiff);
        } else {

          hasMoreHistory = false;
          Logger::log("End of history reached for channel %s",
                      channelId.c_str());
        }

        isFetchingHistory = false;
      });
}

void MessageScreen::openKeyboard() {
  auto &client = Discord::DiscordClient::getInstance();
  if (!client.canSendMessage(channelId)) {
    ScreenManager::getInstance().showToast(TR("message.no_permission"));
    return;
  }

  client.triggerTypingIndicator(channelId);

  SwkbdState swkbd;
  char mybuf[2000];
  mybuf[0] = '\0';

  swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
  swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT | SWKBD_DARKEN_TOP_SCREEN |
                               SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET |
                               SWKBD_ALLOW_POWER | SWKBD_MULTILINE);
  swkbdSetInitialText(&swkbd, mybuf);
  swkbdSetHintText(&swkbd, TR("common.message_hint").c_str());
  swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, TR("common.cancel").c_str(), false);
  swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, TR("common.send").c_str(), true);

  SwkbdButton button = swkbdInputText(&swkbd, mybuf, sizeof(mybuf));

  if (button == SWKBD_BUTTON_RIGHT) {
    if (isForumView) {
      Logger::log("Cannot send messages to Forum directly");
      return;
    }
    std::string content = mybuf;
    size_t first = content.find_first_not_of(" \n\r\t");
    if (first != std::string::npos) {
      content = content.substr(first,
                               content.find_last_not_of(" \n\r\t") - first + 1);
    } else {
      content = "";
    }

    if (!content.empty()) {
      Logger::log("Sending message: %s", content.c_str());

      Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

      Discord::Message optimisticMsg;
      optimisticMsg.id = "pending_" + std::to_string(osGetTime());
      optimisticMsg.content = content;
      optimisticMsg.channelId = channelId;
      optimisticMsg.author = client.getCurrentUser();
      optimisticMsg.timestamp = TR("message.status.sending");

      this->messages.push_back(optimisticMsg);
      rebuildLayoutCache();
      scrollToBottom();

      client.sendMessage(
          channelId, content,
          [this, pendingId = optimisticMsg.id](const Discord::Message &sentMsg,
                                               bool success, int errorCode) {
            std::lock_guard<std::recursive_mutex> lock(messageMutex);
            for (auto &msg : this->messages) {
              if (msg.id == pendingId) {
                if (success) {
                  msg = sentMsg;
                  Logger::log("Updated pending message with confirmed ID: %s",
                              sentMsg.id.c_str());
                } else {
                  msg.timestamp = TR("message.status.failed");
                  Logger::log("Message send failed with code: %d", errorCode);
                }
                break;
              }
            }
          });
    }
  }
}

void MessageScreen::showMessageOptions() {
  if (isForumView)
    return;

  if (this->messages.empty() || selectedIndex < 0 ||
      selectedIndex >= (int)this->messages.size())
    return;

  if (isMenuOpen)
    return;

  const Discord::Message &msg = this->messages[selectedIndex];
  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  bool isMine = (msg.author.id == client.getCurrentUser().id);

  menuOptions.clear();
  menuActions.clear();

  auto addOption = [&](const std::string &actionId,
                       const std::string &i18nKey) {
    menuActions.push_back(actionId);
    menuOptions.push_back(Core::I18n::getInstance().get(i18nKey));
  };

  bool canSend = client.canSendMessage(channelId);

  if (canSend) {
    addOption("Reply", "message.menu.reply");
  }

  if (isMine && canSend) {
    addOption("Edit", "message.menu.edit");
  }

  if (isMine) {
    addOption("Delete", "message.menu.delete");
  }

  bool hasFailed = false;
  for (const auto &attach : msg.attachments) {
    std::string url = attach.proxy_url.empty() ? attach.url : attach.proxy_url;
    if (ImageManager::getInstance().getImageInfo(url).failed ||
        ImageManager::getInstance().getImageInfo(attach.url).failed) {
      hasFailed = true;
      break;
    }
  }
  if (!hasFailed) {
    for (const auto &sticker : msg.stickers) {
      std::string ext = (sticker.format_type == 4) ? ".gif" : ".png";
      std::string url =
          "https://cdn.discordapp.com/stickers/" + sticker.id + ext;
      if (ImageManager::getInstance().getImageInfo(url).failed) {
        hasFailed = true;
        break;
      }
    }
  }
  if (!hasFailed) {
    for (const auto &embed : msg.embeds) {
      if (!embed.image_url.empty() &&
          (ImageManager::getInstance().getImageInfo(embed.image_url).failed ||
           ImageManager::getInstance()
               .getImageInfo(embed.image_proxy_url)
               .failed)) {
        hasFailed = true;
        break;
      }
      if (!embed.thumbnail_url.empty() &&
          (ImageManager::getInstance()
               .getImageInfo(embed.thumbnail_url)
               .failed ||
           ImageManager::getInstance()
               .getImageInfo(embed.thumbnail_proxy_url)
               .failed)) {
        hasFailed = true;
        break;
      }
    }
  }

  if (hasFailed) {
    addOption("Retry", "message.menu.retry");
  }

  addOption("Cancel", "message.menu.cancel");

  isMenuOpen = true;
  menuIndex = 0;
}

void MessageScreen::scrollToBottom() {
  if (this->messages.empty())
    return;

  selectedIndex = this->messages.size() - 1;

  const float SCREEN_HEIGHT = 230.0f;
  float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
  targetScrollY = maxScroll;
  currentScrollY = maxScroll;

  showNewMessageIndicator = false;
}

void MessageScreen::rebuildLayoutCache() {
  messagePositions.clear();
  messageHeights.clear();

  if (messages.empty()) {
    totalContentHeight = 0.0f;
    return;
  }

  float y = 10.0f;
  std::string lastDate = "";

  for (size_t i = 0; i < this->messages.size(); i++) {
    bool showHeader = (i == 0) || !MessageUtils::canGroupWithPrevious(
                                      this->messages[i], this->messages[i - 1]);

    if (this->messages[i].timestamp != "Sending...") {
      std::string currDate =
          MessageUtils::getLocalDateString(this->messages[i].timestamp);

      if (currDate != lastDate) {
        y += 28.0f;
        lastDate = currDate;
      }
    }

    messagePositions.push_back(y);
    float h = calculateMessageHeight(this->messages[i], showHeader);
    messageHeights.push_back(h);

    y += h;
  }

  totalContentHeight = y + 2.0f;

  const float SCREEN_HEIGHT = 230.0f;
  float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);

  if (targetScrollY > maxScroll) {
    targetScrollY = maxScroll;
  }
  if (currentScrollY > maxScroll) {
    currentScrollY = maxScroll;
  }
}

void MessageScreen::ensureSelectionVisible() {
  if (selectedIndex < 0 || selectedIndex >= (int)this->messages.size())
    return;

  if (messagePositions.empty())
    return;

  const float SCREEN_HEIGHT = 230.0f;
  const float TOP_MARGIN = 20.0f;
  const float BOTTOM_MARGIN = 20.0f;

  float msgY = messagePositions[selectedIndex];
  float msgH = messageHeights[selectedIndex];

  float visibleTop = targetScrollY;
  float visibleBottom = targetScrollY + SCREEN_HEIGHT;

  if (msgY < visibleTop + TOP_MARGIN) {
    targetScrollY = msgY - TOP_MARGIN;
  } else if (msgY + msgH > visibleBottom - BOTTOM_MARGIN) {
    targetScrollY = (msgY + msgH) - SCREEN_HEIGHT + BOTTOM_MARGIN;
  }

  float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
  targetScrollY = std::clamp(targetScrollY, 0.0f, maxScroll);
}

void MessageScreen::renderMenu() {
  if (!isMenuOpen)
    return;

  drawOverlay(0.98f);

  float menuW = 200.0f;
  float menuH = menuOptions.size() * 25.0f + 9.0f;
  float menuX = (400.0f - menuW) / 2.0f;
  float menuY = (240.0f - menuH) / 2.0f;

  drawPopupBackground(menuX, menuY, menuW, menuH, 0.99f);

  for (size_t i = 0; i < menuOptions.size(); i++) {
    float itemY = menuY + 6.0f + i * 25.0f;
    u32 color = ScreenManager::colorText();

    bool isSelected = (i == (size_t)menuIndex);
    drawPopupMenuItem(menuX + 5.0f, itemY, menuW - 10.0f, 22.0f, 0.995f,
                      isSelected, ScreenManager::colorSelection());

    if (isSelected) {
      color = ScreenManager::colorWhite();
    }

    drawCenteredText(itemY + 4.0f, 0.996f, 0.5f, 0.5f, color, menuOptions[i],
                     400.0f);
  }
}

float MessageScreen::calculateEmbedHeight(const Discord::Embed &embed,
                                          float maxWidth) {

  size_t embedHash = 0;
  embedHash ^= std::hash<std::string>{}(embed.title);
  embedHash ^= std::hash<std::string>{}(embed.description);
  embedHash ^= std::hash<std::string>{}(embed.author_name);
  embedHash ^= std::hash<std::string>{}(embed.footer_text);
  embedHash ^= std::hash<int>{}(embed.fields.size());
  embedHash ^= std::hash<float>{}(maxWidth);

  auto cacheIt = embedHeightCache.find(embedHash);
  if (cacheIt != embedHeightCache.end()) {
    return cacheIt->second;
  }

  bool hasImage = !embed.image_url.empty();
  bool hasThumbnail = !embed.thumbnail_url.empty();

  bool isLargeThumbnail =
      (hasThumbnail && embed.thumbnail_width >= 160 &&
       (float)embed.thumbnail_width > (float)embed.thumbnail_height * 1.2f);
  bool isMedia =
      (embed.type == "image" || embed.type == "gifv" || embed.type == "video" ||
       embed.type == "article" || isLargeThumbnail);

  bool isSimpleMedia = isMedia && embed.title.empty() &&
                       embed.description.empty() && embed.fields.empty() &&
                       embed.author_name.empty() && (hasImage || hasThumbnail);

  bool showThumbnailOnRight = !isSimpleMedia && hasThumbnail && !isMedia;
  float pixelWidth = maxWidth - (showThumbnailOnRight ? 76.0f : 16.0f);
  float h = isSimpleMedia ? 0.0f : 10.0f;

  if (!embed.provider_name.empty())
    h += 11.0f;
  if (!embed.author_name.empty()) {
    auto lines =
        MessageUtils::wrapText(embed.author_name, pixelWidth, 0.38f, false);
    h += lines.size() * 11.0f;
  }
  if (!embed.title.empty()) {
    auto lines = MessageUtils::wrapText(embed.title, pixelWidth, 0.42f, false);
    h += lines.size() * 14.0f;
  }
  if (!embed.description.empty()) {
    auto lines =
        MessageUtils::wrapText(embed.description, pixelWidth, 0.36f, false);
    h += lines.size() * 11.0f;
  }

  for (const auto &field : embed.fields) {
    auto nLines = MessageUtils::wrapText(field.name, pixelWidth, 0.35f, false);
    h += nLines.size() * 11.0f;
    auto vLines = MessageUtils::wrapText(field.value, pixelWidth, 0.34f, false);
    h += vLines.size() * 11.0f;
    h += 2.0f;
  }

  if (!embed.footer_text.empty()) {
    auto lines =
        MessageUtils::wrapText(embed.footer_text, pixelWidth, 0.30f, false);
    h += lines.size() * 10.0f;
  }

  if (showThumbnailOnRight)
    h = std::max(h, 72.0f);

  if (hasImage || (isMedia && hasThumbnail)) {
    int w = hasImage ? embed.image_width : embed.thumbnail_width;
    int h_img = hasImage ? embed.image_height : embed.thumbnail_height;

    float availableMaxWidth = maxWidth - (isSimpleMedia ? 0.0f : 16.0f);
    availableMaxWidth = std::min(availableMaxWidth, 348.0f);
    float drawW = availableMaxWidth;
    if (w > 0 && w < 160) {
      drawW = (float)w;
    }

    float imgH = 100.0f;
    if (w > 0 && h_img > 0) {
      float aspect = (float)w / h_img;
      imgH = drawW / aspect;
      if (imgH > 220.0f) {
        imgH = 220.0f;
      }
    } else {
      imgH = drawW * 0.5625f;
    }
    h += imgH + 4.0f;
  }

  embedHeightCache[embedHash] = h;

  return h;
}

float MessageScreen::renderEmbed(const Discord::Embed &embed, float x, float y,
                                 float maxWidth) {
  bool hasImage = !embed.image_url.empty();
  bool hasThumbnail = !embed.thumbnail_url.empty();

  bool isLargeThumbnail =
      (hasThumbnail && embed.thumbnail_width >= 160 &&
       (float)embed.thumbnail_width > (float)embed.thumbnail_height * 1.2f);
  bool isMedia =
      (embed.type == "image" || embed.type == "gifv" || embed.type == "video" ||
       embed.type == "article" || isLargeThumbnail);

  bool isSimpleMedia = isMedia && embed.title.empty() &&
                       embed.description.empty() && embed.fields.empty() &&
                       embed.author_name.empty() && (hasImage || hasThumbnail);

  u32 embedColor = embed.color != 0 ? C2D_Color32((embed.color >> 16) & 0xFF,
                                                  (embed.color >> 8) & 0xFF,
                                                  embed.color & 0xFF, 255)
                                    : C2D_Color32(32, 102, 148, 255);
  float embedH = calculateEmbedHeight(embed, maxWidth);

  bool showThumbnailOnRight = !isSimpleMedia && hasThumbnail && !isMedia;
  float pixelWidth = maxWidth - (showThumbnailOnRight ? 76.0f : 16.0f);

  if (!isSimpleMedia) {
    C2D_DrawRectSolid(x, y, 0.4f, maxWidth, embedH,
                      ScreenManager::colorEmbed());
    C2D_DrawRectSolid(x, y, 0.45f, 4.0f, embedH, embedColor);
  }

  float currentY = y + (isSimpleMedia ? 0.0f : 5.0f);
  float textX = x + (isSimpleMedia ? 0.0f : 8.0f);

  if (!embed.provider_name.empty()) {
    drawText(textX, currentY, 0.5f, 0.32f, 0.32f,
             ScreenManager::colorTextMuted(), embed.provider_name);
    currentY += 11.0f;
  }
  if (!embed.author_name.empty()) {
    auto lines =
        MessageUtils::wrapText(embed.author_name, pixelWidth, 0.38f, false);
    for (const auto &line : lines) {
      drawRichText(textX, currentY, 0.5f, 0.38f, 0.38f,
                   ScreenManager::colorText(), line);
      currentY += 11.0f;
    }
  }
  if (!embed.title.empty()) {
    auto lines = MessageUtils::wrapText(embed.title, pixelWidth, 0.42f, false);
    for (const auto &line : lines) {
      drawRichText(textX, currentY, 0.5f, 0.42f, 0.42f,
                   ScreenManager::colorText(), line);
      currentY += 14.0f;
    }
  }
  if (!embed.description.empty()) {
    auto lines =
        MessageUtils::wrapText(embed.description, pixelWidth, 0.36f, false);
    for (const auto &line : lines) {
      drawRichText(textX, currentY, 0.5f, 0.36f, 0.36f,
                   ScreenManager::colorText(), line);
      currentY += 11.0f;
    }
  }
  for (const auto &field : embed.fields) {
    auto nLines = MessageUtils::wrapText(field.name, pixelWidth, 0.35f, false);
    for (const auto &line : nLines) {
      drawRichText(textX, currentY, 0.5f, 0.35f, 0.35f,
                   ScreenManager::colorText(), line);
      currentY += 11.0f;
    }
    auto vLines = MessageUtils::wrapText(field.value, pixelWidth, 0.34f, false);
    for (const auto &line : vLines) {
      drawRichText(textX, currentY, 0.5f, 0.34f, 0.34f,
                   ScreenManager::colorTextMuted(), line);
      currentY += 11.0f;
    }
    currentY += 2.0f;
  }
  if (!embed.footer_text.empty()) {
    auto lines =
        MessageUtils::wrapText(embed.footer_text, pixelWidth, 0.30f, false);
    for (const auto &line : lines) {
      drawRichText(textX, currentY, 0.5f, 0.30f, 0.30f,
                   ScreenManager::colorTextMuted(), line);
      currentY += 10.0f;
    }
  }

  if (showThumbnailOnRight) {
    float thumbSize = 64.0f;
    float thumbX = x + maxWidth - (thumbSize + 4.0f);
    float thumbY = y + 5.0f;
    std::string thumbUrl = embed.thumbnail_url;

    auto info = UI::ImageManager::getInstance().getImageInfo(thumbUrl);
    if (info.tex) {
      float scale =
          thumbSize / std::max((float)info.originalW, (float)info.originalH);
      float drawW = info.originalW * scale;
      float drawH = info.originalH * scale;
      float offsetX = (thumbSize - drawW) / 2.0f;
      float offsetY = (thumbSize - drawH) / 2.0f;

      float uMax = (float)info.originalW / info.tex->width;
      float vMax = (float)info.originalH / info.tex->height;
      Tex3DS_SubTexture subtex = {
          (u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax,
          1.0f - vMax};
      C2D_Image img = {info.tex, &subtex};
      C2D_DrawImageAt(img, thumbX + offsetX, thumbY + offsetY, 0.48f, nullptr,
                      scale, scale);
    } else {
      UI::ImageManager::getInstance().prefetch(
          thumbUrl, embed.thumbnail_width, embed.thumbnail_height,
          Network::RequestPriority::INTERACTIVE);
      C2D_DrawRectSolid(thumbX, thumbY, 0.48f, thumbSize, thumbSize,
                        ScreenManager::colorEmbedMedia());
    }
  }

  if (hasImage || (isMedia && hasThumbnail)) {
    std::string mediaUrl =
        hasImage
            ? (!embed.image_proxy_url.empty() ? embed.image_proxy_url
                                              : embed.image_url)
            : (!embed.thumbnail_proxy_url.empty() ? embed.thumbnail_proxy_url
                                                  : embed.thumbnail_url);
    int mediaW = hasImage ? embed.image_width : embed.thumbnail_width;
    int mediaH = hasImage ? embed.image_height : embed.thumbnail_height;

    float availableMaxWidth = maxWidth - (isSimpleMedia ? 0.0f : 16.0f);
    availableMaxWidth = std::min(availableMaxWidth, 348.0f);
    float drawW = availableMaxWidth;
    if (mediaW > 0 && mediaW < 160) {
      drawW = (float)mediaW;
    }

    float drawH = 100.0f;
    if (mediaW > 0 && mediaH > 0) {
      float aspect = (float)mediaW / mediaH;
      drawH = drawW / aspect;
      if (drawH > 220.0f) {
        drawH = 220.0f;
        drawW = drawH * aspect;
      }
    } else {
      drawH = drawW * 0.5625f;
    }

    auto info = ImageManager::getInstance().getImageInfo(mediaUrl);
    if (info.tex) {
      float uMax = (float)info.originalW / info.tex->width;
      float vMax = (float)info.originalH / info.tex->height;
      Tex3DS_SubTexture subtex = {
          (u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax,
          1.0f - vMax};
      C2D_Image img = {info.tex, &subtex};
      C2D_DrawImageAt(img, textX, currentY, 0.49f, nullptr,
                      drawW / info.originalW, drawH / info.originalH);
    } else if (info.failed) {
      u32 errorBg = C2D_Color32(60, 40, 40, 255);
      C2D_DrawRectSolid(textX, currentY, 0.49f, drawW, drawH, errorBg);
      drawText(textX + 5, currentY + (drawH / 2) - 6, 0.5f, 0.35f, 0.35f,
               ScreenManager::colorError(), TR("message.image_failed"));
    } else {
      ImageManager::getInstance().prefetch(
          mediaUrl, mediaW, mediaH, Network::RequestPriority::INTERACTIVE);
      C2D_DrawRectSolid(textX, currentY, 0.49f, drawW, drawH,
                        ScreenManager::colorEmbedMedia());
      drawText(textX + 5, currentY + (drawH / 2) - 6, 0.5f, 0.35f, 0.35f,
               ScreenManager::colorTextMuted(), TR("common.loading"));
    }
    currentY += drawH + 4.0f;
  }

  return currentY - y;
}

void MessageScreen::catchUpMessages() {
  if (channelId.empty())
    return;

  Discord::DiscordClient::getInstance().fetchMessagesAsync(
      channelId, 50, [this](const std::vector<Discord::Message> &fetched) {
        if (fetched.empty())
          return;

        std::lock_guard<std::recursive_mutex> lock(messageMutex);

        std::string latestRealId;
        for (auto it = this->messages.rbegin(); it != this->messages.rend();
             ++it) {
          if (it->id.substr(0, 8) != "pending_") {
            latestRealId = it->id;
            break;
          }
        }

        if (latestRealId.empty()) {

          this->messages = fetched;
          std::reverse(this->messages.begin(), this->messages.end());
          rebuildLayoutCache();
          return;
        }

        int foundIndex = -1;
        for (size_t i = 0; i < fetched.size(); i++) {
          if (fetched[i].id == latestRealId) {
            foundIndex = i;
            break;
          }
        }

        bool addedAny = false;
        if (foundIndex != -1) {

          for (int i = foundIndex - 1; i >= 0; i--) {
            messages.push_back(fetched[i]);
            addedAny = true;
          }
        } else {
          for (int i = (int)fetched.size() - 1; i >= 0; i--) {
            if (fetched[i].id > latestRealId) {
              this->messages.push_back(fetched[i]);
              addedAny = true;
            }
          }
        }

        if (addedAny) {
          Logger::log("[UI] Merged %d new messages from catch-up", addedAny);

          const float SCREEN_HEIGHT = 230.0f;
          float oldMaxScroll =
              std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
          bool wasAtBottom = (targetScrollY >= oldMaxScroll - 5.0f);

          rebuildLayoutCache();

          if (wasAtBottom) {
            selectedIndex = this->messages.size() - 1;
            scrollToBottom();
          } else {
            showNewMessageIndicator = true;
            newMessageCount += addedAny;
          }
        }
      });
}

} // namespace UI
