#include "ui/screen_manager.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "log.h"
#include "ui/about_screen.h"
#include "ui/dm_screen.h"
#include "ui/emoji_manager.h"
#include "ui/forum_screen.h"
#include "ui/image_manager.h"
#include "ui/login_screen.h"
#include "ui/message_screen.h"
#include "ui/server_list_screen.h"
#include "ui/settings_screen.h"
#include "ui/text_measure_cache.h"
#include "utils/message_utils.h"
#include "utils/utf8_utils.h"

namespace UI {

static C2D_TextBuf textBuf = nullptr;
static C2D_TextBuf debugTextBuf = nullptr;
static C2D_TextBuf layoutTextBuf = nullptr;

Screen::Screen() : exitRequested(false) {}

ScreenManager &ScreenManager::getInstance() {
  static ScreenManager instance;
  return instance;
}

void ScreenManager::init() {
  Config::getInstance().loadTheme();
  topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

  if (!textBuf) {
    textBuf = C2D_TextBufNew(32768);
  }
  if (!debugTextBuf) {
    debugTextBuf = C2D_TextBufNew(16384);
  }
  if (!layoutTextBuf) {
    layoutTextBuf = C2D_TextBufNew(32768);
  }

  debugOverlayEnabled = false;

  Logger::log("[UI] Screen manager initialized");

  if (Discord::DiscordClient::getInstance().getState() ==
      Discord::ConnectionState::READY) {
    setScreen(ScreenType::GUILD_LIST);
  } else {
    setScreen(ScreenType::LOGIN);
  }
}

void ScreenManager::shutdown() {
  if (currentScreen) {
    currentScreen->onExit();
    currentScreen.reset();
  }

  if (textBuf) {
    C2D_TextBufDelete(textBuf);
    textBuf = nullptr;
  }
  if (debugTextBuf) {
    C2D_TextBufDelete(debugTextBuf);
    debugTextBuf = nullptr;
  }
  if (layoutTextBuf) {
    C2D_TextBufDelete(layoutTextBuf);
    layoutTextBuf = nullptr;
  }

  Logger::log("[UI] Screen manager shutdown");
}

void ScreenManager::setScreen(ScreenType type) {
  if (currentScreen) {
    currentScreen->onExit();
  }

  if (type == ScreenType::LOGIN || type == ScreenType::GUILD_LIST ||
      type == ScreenType::ADD_ACCOUNT || type == ScreenType::DM_LIST) {
    screenHistory.clear();
  }

  currentType = type;

  if (type == ScreenType::LOGIN || type == ScreenType::ADD_ACCOUNT) {
    hamburgerMenu.reset();
  }

  switch (type) {
  case ScreenType::LOGIN:
    expandedFolders.clear();

    currentScreen = std::make_unique<LoginScreen>();
    break;
  case ScreenType::GUILD_LIST:
    currentScreen = std::make_unique<ServerListScreen>();
    break;
  case ScreenType::MESSAGES: {
    auto &client = Discord::DiscordClient::getInstance();
    std::string channelId = client.getSelectedChannelId();
    std::string channelName = TR("common.channel");
    for (const auto &g : client.getGuilds()) {
      for (const auto &ch : g.channels) {
        if (ch.id == channelId) {
          channelName = ch.name;
          goto found;
        }
      }
    }
    for (const auto &ch : client.getPrivateChannels()) {
      if (ch.id == channelId) {
        channelName = ch.name;
        break;
      }
    }
  found:
    currentScreen = std::make_unique<MessageScreen>(channelId, channelName);
    break;
  }
  case ScreenType::ADD_ACCOUNT:
    currentScreen = std::make_unique<LoginScreen>();
    break;
  case ScreenType::FORUM_CHANNEL: {
    auto &client = Discord::DiscordClient::getInstance();
    std::string channelId = client.getSelectedChannelId();
    std::string channelName = TR("common.forum");
    for (const auto &g : client.getGuilds()) {
      for (const auto &ch : g.channels) {
        if (ch.id == channelId) {
          channelName = ch.name;
          break;
        }
      }
    }
    currentScreen = std::make_unique<ForumScreen>(channelId, channelName);
    break;
  }
  case ScreenType::SETTINGS:
    currentScreen = std::make_unique<SettingsScreen>();
    break;
  case ScreenType::DM_LIST:
    currentScreen = std::make_unique<DmScreen>();
    break;
  case ScreenType::ABOUT:
    currentScreen = std::make_unique<AboutScreen>();
    break;
  }

  if (currentScreen) {
    currentScreen->onEnter();
  }
}

void ScreenManager::pushScreen(ScreenType type) {
  if (currentType != type) {
    screenHistory.push_back(currentType);
  }
  setScreen(type);
}

void ScreenManager::returnToPreviousScreen() {
  if (screenHistory.empty()) {
    if (currentType != ScreenType::GUILD_LIST &&
        currentType != ScreenType::LOGIN) {
      setScreen(ScreenType::GUILD_LIST);
    }
    return;
  }

  ScreenType prev = screenHistory.back();
  screenHistory.pop_back();

  setScreen(prev);
}

void ScreenManager::update() {
  ImageManager::getInstance().update();
  EmojiManager::getInstance().update();
  Discord::AvatarCache::getInstance().update();

  hamburgerMenu.update();

  if (layoutTextBuf) {
    C2D_TextBufClear(layoutTextBuf);
  }

  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();

  if (kDown & KEY_START) {
    appExitRequested = true;
    return;
  }

  bool shouldBlockScreen = !hamburgerMenu.isClosed();

  auto &client = Discord::DiscordClient::getInstance();
  bool isConnecting =
      (client.getState() == Discord::ConnectionState::CONNECTING ||
       client.getState() == Discord::ConnectionState::AUTHENTICATING);
  bool hideMenu = (currentType == ScreenType::LOGIN) ||
                  (currentType == ScreenType::ADD_ACCOUNT && isConnecting);

  if (!hideMenu) {
    if (kDown & KEY_SELECT) {
      hamburgerMenu.toggle();
      shouldBlockScreen = true;
    }

    touchPosition touch;
    hidTouchRead(&touch);
    if (kDown & KEY_TOUCH) {
      if (touch.px < 40 && touch.py < 40) {
        hamburgerMenu.toggle();
        shouldBlockScreen = true;
      }
    }
  }

  if (!shouldBlockScreen) {
    if (currentScreen) {
      currentScreen->update();
    }
  }

  if ((kHeld & KEY_L) && (kDown & KEY_R)) {
    toggleDebugOverlay();
    Logger::log("Debug overlay toggled: %s",
                debugOverlayEnabled ? "ON" : "OFF");
  }

  if (toastTimer > 0) {
    toastTimer--;
  }
}

void ScreenManager::render() {
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

  if (textBuf) {
    C2D_TextBufClear(textBuf);
  }
  if (debugTextBuf) {
    C2D_TextBufClear(debugTextBuf);
  }

  C2D_TargetClear(topTarget, colorBackground());
  C2D_SceneBegin(topTarget);

  if (currentScreen) {
    currentScreen->renderTop(topTarget);
  }

  auto &client = Discord::DiscordClient::getInstance();
  bool isConnecting =
      (client.getState() == Discord::ConnectionState::CONNECTING ||
       client.getState() == Discord::ConnectionState::AUTHENTICATING);
  bool hideMenu = (currentType == ScreenType::LOGIN) ||
                  (currentType == ScreenType::ADD_ACCOUNT && isConnecting);

  if (!hideMenu) {
    hamburgerMenu.render();
  }

  if (debugOverlayEnabled) {
    renderDebugOverlay();
  }

  C2D_TargetClear(bottomTarget, colorBackground());
  C2D_SceneBegin(bottomTarget);

  if (currentScreen) {
    currentScreen->renderBottom(bottomTarget);
  }

  if (!hideMenu) {
    drawHamburgerButton();
  }

  if (toastTimer > 0) {
    drawToast();
  }

  C3D_FrameEnd(0);
}

void ScreenManager::toggleDebugOverlay() {
  debugOverlayEnabled = !debugOverlayEnabled;
}

void ScreenManager::renderDebugOverlay() {
  std::vector<std::string> logs = Logger::getRecentLogs();
  float y = 5.0f;
  float lineHeight = 10.0f;

  for (const auto &line : logs) {
    if (y + lineHeight > 240)
      break;

    C2D_Text text;
    C2D_TextParse(&text, debugTextBuf, line.c_str());
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, 5.0f, y, 1.0f, 0.4f, 0.4f,
                 C2D_Color32(0, 255, 0, 255));

    y += lineHeight;
  }
}

void ScreenManager::drawHamburgerButton() {
  u32 color = colorText();
  float x = 12.0f;
  float y = 11.0f;
  float w = 18.0f;
  float h = 2.0f;
  float gap = 5.0f;
  float z = 1.0f;
  float r = 1.0f;

  drawRoundedRect(x, y, z, w, h, r, color);
  drawRoundedRect(x, y + gap, z, w, h, r, color);
  drawRoundedRect(x, y + gap * 2, z, w, h, r, color);
}

void ScreenManager::showToast(const std::string &message) {
  toastMessage = message;
  toastTimer = 90;
}

void ScreenManager::drawToast() {
  float w = measureText(toastMessage, 0.5f, 0.5f) + 24.0f;
  float h = 32.0f;
  float x = (320.0f - w) / 2.0f;
  float y = 180.0f;
  float z = 0.95f;

  u32 bg = C2D_Color32(40, 40, 45, 235);
  drawRoundedRect(x, y, z, w, h, 8.0f, bg);
  drawRoundedRect(x + 4, y + h - 2.0f, z + 0.01f, w - 8.0f, 1.5f, 0.75f,
                  colorSelection());

  C2D_SceneBegin(bottomTarget);
  drawCenteredText(y + 9.0f, z + 0.02f, 0.5f, 0.5f, colorWhite(), toastMessage,
                   320.0f);
}

void drawText(float x, float y, float z, float scaleX, float scaleY, u32 color,
              const std::string &rawText) {
  std::string text = Utils::Utf8::sanitizeText(rawText);

  if (!textBuf)
    return;

  C2D_Text c2dText;
  C2D_TextParse(&c2dText, textBuf, text.c_str());
  C2D_TextOptimize(&c2dText);
  C2D_DrawText(&c2dText, C2D_WithColor, x, y, z, scaleX, scaleY, color);
}

void drawCenteredText(float y, float z, float scaleX, float scaleY, u32 color,
                      const std::string &rawText, float screenWidth) {
  std::string text = Utils::Utf8::sanitizeText(rawText);

  if (!textBuf)
    return;

  C2D_Text c2dText;
  C2D_TextParse(&c2dText, textBuf, text.c_str());
  C2D_TextOptimize(&c2dText);

  float width, height;
  C2D_TextGetDimensions(&c2dText, scaleX, scaleY, &width, &height);

  float x = (screenWidth - width) / 2.0f;
  C2D_DrawText(&c2dText, C2D_WithColor, x, y, z, scaleX, scaleY, color);
}

float measureTextDirect(const std::string &rawText, float scaleX,
                        float scaleY) {
  std::string text = Utils::Utf8::sanitizeText(rawText);

  if (!layoutTextBuf || text.empty())
    return 0.0f;

  C2D_Text c2dText;
  C2D_TextParse(&c2dText, layoutTextBuf, text.c_str());

  float width, height;
  C2D_TextGetDimensions(&c2dText, scaleX, scaleY, &width, &height);

  C2D_TextBufClear(layoutTextBuf);
  return width;
}

float measureText(const std::string &text, float scaleX, float scaleY) {
  return UI::TextMeasureCache::getInstance().measureText(text, scaleX, scaleY);
}

void drawRoundedRect(float x, float y, float z, float w, float h, float radius,
                     u32 color) {
  if (radius <= 0) {
    C2D_DrawRectSolid(x, y, z, w, h, color);
    return;
  }

  if (radius > w / 2)
    radius = w / 2;
  if (radius > h / 2)
    radius = h / 2;

  C2D_DrawRectSolid(x, y + radius, z, w, h - 2 * radius, color);
  C2D_DrawRectSolid(x + radius, y, z, w - 2 * radius, radius, color);
  C2D_DrawRectSolid(x + radius, y + h - radius, z, w - 2 * radius, radius,
                    color);

  C2D_DrawCircleSolid(x + radius, y + radius, z, radius, color);
  C2D_DrawCircleSolid(x + w - radius, y + radius, z, radius, color);
  C2D_DrawCircleSolid(x + radius, y + h - radius, z, radius, color);
  C2D_DrawCircleSolid(x + w - radius, y + h - radius, z, radius, color);
}

void drawCircle(float x, float y, float z, float radius, u32 color) {
  C2D_DrawCircleSolid(x, y, z, radius, color);
}

void drawRichText(float x, float y, float z, float scaleX, float scaleY,
                  u32 color, const std::string &rawText) {
  std::string text = Utils::Utf8::sanitizeText(rawText);

  if (!textBuf || text.empty())
    return;

  size_t cursor = 0;
  float currentX = x;

  while (cursor < text.length()) {
    if (text[cursor] == '<') {
      size_t start = cursor;
      if (start + 6 < text.length()) {
        bool isAnimated = (text[start + 1] == 'a');
        if (text[start + 1] == ':' || isAnimated) {
          size_t secondColon = text.find(':', start + (isAnimated ? 3 : 2));
          if (secondColon != std::string::npos) {
            size_t closeBracket = text.find('>', secondColon);
            if (closeBracket != std::string::npos) {
              std::string id =
                  text.substr(secondColon + 1, closeBracket - secondColon - 1);
              EmojiManager::EmojiInfo info =
                  EmojiManager::getInstance().getEmojiInfo(id);
              float emojiSize = 28.0f * scaleY;

              if (info.tex) {
                float uMax = (float)info.originalW / info.tex->width;
                float vMax = (float)info.originalH / info.tex->height;

                Tex3DS_SubTexture subtex = {
                    (u16)info.originalW, // width
                    (u16)info.originalH, // height
                    0.0f,                // left
                    1.0f,                // top
                    uMax,                // right
                    1.0f - vMax          // bottom
                };

                const C2D_Image img = {info.tex, &subtex};
                C2D_DrawImageAt(img, currentX, y + 1.0f, z, nullptr,
                                emojiSize / info.originalW,
                                emojiSize / info.originalH);
              } else {
                EmojiManager::getInstance().prefetchEmoji(id);

                std::string name =
                    text.substr(start + (isAnimated ? 3 : 2),
                                secondColon - (start + (isAnimated ? 3 : 2)));
                std::string fallback = ":" + name + ":";
                drawText(currentX, y, z, scaleX, scaleY, color, fallback);

                currentX += measureText(fallback, scaleX, scaleY) -
                            (emojiSize + (2.0f * scaleX));
              }

              currentX += emojiSize + (2.0f * scaleX);
              cursor = closeBracket + 1;
              continue;
            }
          }
        }
      }
    }

    size_t tempCharCursor = cursor;
    uint32_t firstCp = Utils::Utf8::decodeNext(text, tempCharCursor);

    if (Utils::Utf8::isEmoji(firstCp)) {
      size_t seqCursor = cursor;
      std::string sequence = Utils::Utf8::getEmojiSequence(text, seqCursor);
      std::string hex = MessageUtils::getEmojiFilename(sequence);
      EmojiManager::EmojiInfo info =
          EmojiManager::getInstance().getTwemojiInfo(hex);
      float emojiSize = 28.0f * scaleY;

      if (info.tex) {
        float uMax = (float)info.originalW / info.tex->width;
        float vMax = (float)info.originalH / info.tex->height;
        Tex3DS_SubTexture subtex = {
            (u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax,
            1.0f - vMax};
        const C2D_Image img = {info.tex, &subtex};
        C2D_DrawImageAt(img, currentX, y + 1.0f, z, nullptr,
                        emojiSize / info.originalW, emojiSize / info.originalH);
        currentX += emojiSize + (2.0f * scaleX);
      } else {
        std::string clean = Utils::Utf8::sanitizeText(sequence);
        drawText(currentX, y, z, scaleX, scaleY, color, clean);
        currentX += measureText(clean, scaleX, scaleY);
      }
      cursor = seqCursor;
      continue;
    }

    size_t end = cursor;
    while (end < text.length()) {
      if (text[end] == '<') {
        if (end + 6 < text.length()) {
          bool isAnimated = (text[end + 1] == 'a');
          if (text[end + 1] == ':' || isAnimated) {
            size_t secondColon = text.find(':', end + (isAnimated ? 3 : 2));
            if (secondColon != std::string::npos) {
              size_t closeBracket = text.find('>', secondColon);
              if (closeBracket != std::string::npos) {
                break;
              }
            }
          }
        }
      }

      size_t nextC = end;
      uint32_t cp = Utils::Utf8::decodeNext(text, nextC);
      if (Utils::Utf8::isEmoji(cp))
        break;
      end = nextC;
    }

    if (end > cursor) {
      std::string segment = text.substr(cursor, end - cursor);
      drawText(currentX, y, z, scaleX, scaleY, color, segment);
      currentX += measureText(segment, scaleX, scaleY);
      cursor = end;
    } else if (cursor < text.length()) {
      size_t nextC = cursor;
      Utils::Utf8::decodeNext(text, nextC);
      std::string segment = text.substr(cursor, nextC - cursor);
      drawText(currentX, y, z, scaleX, scaleY, color, segment);
      currentX += measureText(segment, scaleX, scaleY);
      cursor = nextC;
    }
  }
}

void drawCenteredRichText(float y, float z, float scaleX, float scaleY,
                          u32 color, const std::string &rawText,
                          float screenWidth) {
  float width = measureRichText(rawText, scaleX, scaleY);
  float x = (screenWidth - width) / 2.0f;
  drawRichText(x, y, z, scaleX, scaleY, color, rawText);
}

float measureRichTextImpl(const std::string &rawText, float scaleX,
                          float scaleY, bool unicodeOnly) {
  std::string text = Utils::Utf8::sanitizeText(rawText);

  if (!layoutTextBuf || text.empty())
    return 0.0f;

  size_t cursor = 0;
  float currentX = 0;

  while (cursor < text.length()) {
    if (!unicodeOnly && text[cursor] == '<') {
      size_t start = cursor;
      if (start + 6 < text.length()) {
        bool isAnimated = (text[start + 1] == 'a');
        if (text[start + 1] == ':' || isAnimated) {
          size_t secondColon = text.find(':', start + (isAnimated ? 3 : 2));
          if (secondColon != std::string::npos) {
            size_t closeBracket = text.find('>', secondColon);
            if (closeBracket != std::string::npos) {
              float emojiSize = 28.0f * scaleY;
              currentX += emojiSize + (2.0f * scaleX);
              cursor = closeBracket + 1;
              continue;
            }
          }
        }
      }
    }

    size_t tempCharCursor = cursor;
    uint32_t firstCp = Utils::Utf8::decodeNext(text, tempCharCursor);

    if (Utils::Utf8::isEmoji(firstCp)) {
      size_t seqCursor = cursor;
      std::string sequence = Utils::Utf8::getEmojiSequence(text, seqCursor);
      std::string hex = MessageUtils::getEmojiFilename(sequence);
      EmojiManager::EmojiInfo info =
          EmojiManager::getInstance().getTwemojiInfo(hex);
      float emojiSize = 28.0f * scaleY;

      if (info.tex) {
        currentX += emojiSize + (2.0f * scaleX);
      } else {
        std::string clean = Utils::Utf8::sanitizeText(sequence);
        currentX += measureText(clean, scaleX, scaleY);
      }
      cursor = seqCursor;
      continue;
    }

    size_t end = cursor;
    while (end < text.length()) {
      if (!unicodeOnly && text[end] == '<') {
        // Check if this looks like a custom emoji
        if (end + 6 < text.length()) {
          bool isAnimated = (text[end + 1] == 'a');
          if (text[end + 1] == ':' || isAnimated) {
            size_t secondColon = text.find(':', end + (isAnimated ? 3 : 2));
            if (secondColon != std::string::npos) {
              size_t closeBracket = text.find('>', secondColon);
              if (closeBracket != std::string::npos) {
                break;
              }
            }
          }
        }
      }

      size_t nextC = end;
      uint32_t cp = Utils::Utf8::decodeNext(text, nextC);
      if (Utils::Utf8::isEmoji(cp))
        break;
      end = nextC;
    }

    if (end > cursor) {
      std::string segment = text.substr(cursor, end - cursor);
      currentX += measureText(segment, scaleX, scaleY);
      cursor = end;
    } else if (cursor < text.length()) {
      size_t nextC = cursor;
      Utils::Utf8::decodeNext(text, nextC);
      std::string segment = text.substr(cursor, nextC - cursor);
      currentX += measureText(segment, scaleX, scaleY);
      cursor = nextC;
    }
  }

  return currentX;
}

float measureRichText(const std::string &rawText, float scaleX, float scaleY) {
  return measureRichTextImpl(rawText, scaleX, scaleY, false);
}

std::string getTruncatedText(const std::string &text, float maxWidth,
                             float scaleX, float scaleY) {
  if (measureText(text, scaleX, scaleY) <= maxWidth)
    return text;

  std::vector<size_t> offsets;
  for (size_t i = 0; i < text.length();) {
    offsets.push_back(i);
    unsigned char c = (unsigned char)text[i];
    if (c < 0x80)
      i += 1;
    else if ((c & 0xE0) == 0xC0)
      i += 2;
    else if ((c & 0xF0) == 0xE0)
      i += 3;
    else if ((c & 0xF4) == 0xF0)
      i += 4;
    else
      i += 1;
  }

  int low = 0;
  int high = (int)offsets.size() - 1;
  int best = 0;

  while (low <= high) {
    int mid = low + (high - low) / 2;
    std::string test = text.substr(0, offsets[mid]) + "...";
    if (measureText(test, scaleX, scaleY) <= maxWidth) {
      best = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  return text.substr(0, offsets[best]) + "...";
}

std::string getTruncatedRichText(const std::string &rawText, float maxWidth,
                                 float scaleX, float scaleY) {
  if (measureRichText(rawText, scaleX, scaleY) <= maxWidth)
    return rawText;

  std::vector<size_t> offsets;
  for (size_t i = 0; i < rawText.length();) {
    offsets.push_back(i);
    unsigned char c = (unsigned char)rawText[i];
    if (c < 0x80)
      i += 1;
    else if ((c & 0xE0) == 0xC0)
      i += 2;
    else if ((c & 0xF0) == 0xE0)
      i += 3;
    else if ((c & 0xF4) == 0xF0)
      i += 4;
    else
      i += 1;
  }

  int low = 0;
  int high = (int)offsets.size() - 1;
  int best = 0;

  while (low <= high) {
    int mid = low + (high - low) / 2;
    std::string test = rawText.substr(0, offsets[mid]) + "...";
    if (measureRichText(test, scaleX, scaleY) <= maxWidth) {
      best = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  return rawText.substr(0, offsets[best]) + "...";
}

void drawRichTextUnicodeOnly(float x, float y, float z, float scaleX,
                             float scaleY, u32 color,
                             const std::string &rawText) {
  std::string text = Utils::Utf8::sanitizeText(rawText);

  if (!textBuf || text.empty())
    return;

  size_t cursor = 0;
  float currentX = x;

  while (cursor < text.length()) {
    size_t tempCharCursor = cursor;
    uint32_t firstCp = Utils::Utf8::decodeNext(text, tempCharCursor);

    if (Utils::Utf8::isEmoji(firstCp)) {
      size_t seqCursor = cursor;
      std::string sequence = Utils::Utf8::getEmojiSequence(text, seqCursor);
      std::string hex = MessageUtils::getEmojiFilename(sequence);
      EmojiManager::EmojiInfo info =
          EmojiManager::getInstance().getTwemojiInfo(hex);
      float emojiSize = 28.0f * scaleY;

      if (info.tex) {
        Tex3DS_SubTexture subtex;
        subtex.width = (u16)info.originalW;
        subtex.height = (u16)info.originalH;
        subtex.left = 0.0f;
        subtex.top = 0.0f;
        subtex.right = (float)info.originalW / info.tex->width;
        subtex.bottom = (float)info.originalH / info.tex->height;

        const C2D_Image img = {info.tex, &subtex};
        C2D_DrawImageAt(img, currentX, y + 1.0f, z, nullptr,
                        emojiSize / info.originalW, emojiSize / info.originalH);
        currentX += emojiSize + (2.0f * scaleX);
      } else {
        std::string clean = Utils::Utf8::sanitizeText(sequence);
        drawText(currentX, y, z, scaleX, scaleY, color, clean);
        currentX += measureText(clean, scaleX, scaleY);
      }
      cursor = seqCursor;
    } else {
      size_t end = cursor;
      while (end < text.length()) {
        size_t nextC = end;
        uint32_t cp = Utils::Utf8::decodeNext(text, nextC);
        if (Utils::Utf8::isEmoji(cp))
          break;
        end = nextC;
      }

      if (end > cursor) {
        std::string segment = text.substr(cursor, end - cursor);
        drawText(currentX, y, z, scaleX, scaleY, color, segment);
        currentX += measureText(segment, scaleX, scaleY);
        cursor = end;
      } else if (cursor < text.length()) {
        size_t nextC = cursor;
        Utils::Utf8::decodeNext(text, nextC);
        std::string segment = text.substr(cursor, nextC - cursor);
        drawText(currentX, y, z, scaleX, scaleY, color, segment);
        currentX += measureText(segment, scaleX, scaleY);
        cursor = nextC;
      }
    }
  }
}

float measureRichTextUnicodeOnly(const std::string &rawText, float scaleX,
                                 float scaleY) {
  return measureRichTextImpl(rawText, scaleX, scaleY, true);
}

u32 ScreenManager::colorBackground() {
  return Config::getInstance().getTheme().background;
}
u32 ScreenManager::colorBackgroundDark() {
  return Config::getInstance().getTheme().backgroundDark;
}
u32 ScreenManager::colorBackgroundLight() {
  return Config::getInstance().getTheme().backgroundLight;
}
u32 ScreenManager::colorPrimary() {
  return Config::getInstance().getTheme().primary;
}
u32 ScreenManager::colorText() { return Config::getInstance().getTheme().text; }
u32 ScreenManager::colorTextMuted() {
  return Config::getInstance().getTheme().textMuted;
}
u32 ScreenManager::colorSuccess() {
  return Config::getInstance().getTheme().success;
}
u32 ScreenManager::colorError() {
  return Config::getInstance().getTheme().error;
}

u32 ScreenManager::colorInput() {
  int type = Config::getInstance().getThemeType();
  if (type == 1) {
    return C2D_Color32(230, 230, 230, 255);
  } else if (type == 2) {
    return C2D_Color32(10, 10, 10, 255);
  }
  return C2D_Color32(32, 34, 37, 255);
}
u32 ScreenManager::colorBoost() { return C2D_Color32(255, 115, 250, 255); }
u32 ScreenManager::colorLink() { return C2D_Color32(73, 182, 254, 255); }

u32 ScreenManager::colorSeparator() { return colorTextMuted(); }

u32 ScreenManager::colorHeaderGlass() {
  u32 bg = colorBackgroundDark();
  u8 r = (bg >> 0) & 0xFF;
  u8 g = (bg >> 8) & 0xFF;
  u8 b = (bg >> 16) & 0xFF;
  return C2D_Color32(r, g, b, 230);
}

u32 ScreenManager::colorHeaderBorder() {
  return C2D_Color32(255, 255, 255, 30);
}

u32 ScreenManager::colorSelection() { return colorPrimary(); }

u32 ScreenManager::colorOverlay() { return C2D_Color32(0, 0, 0, 150); }

u32 ScreenManager::colorWhite() { return C2D_Color32(255, 255, 255, 255); }

u32 ScreenManager::colorEmbed() {
  return Config::getInstance().getTheme().embed;
}

u32 ScreenManager::colorEmbedMedia() {
  return Config::getInstance().getTheme().embedMedia;
}

u32 ScreenManager::colorReaction() {
  return Config::getInstance().getTheme().reaction;
}

u32 ScreenManager::colorReactionMe() {
  return Config::getInstance().getTheme().reactionMe;
}

void ScreenManager::resetSelection() {
  lastServerIndex = 0;
  lastServerScroll = 0;
  lastChannelIndex.clear();
  lastChannelScroll.clear();
  lastForumIndex.clear();
  lastForumScroll.clear();
  selectedGuildId = "";
  expandedFolders.clear();
}

void ScreenManager::clearCaches() {
  Discord::AvatarCache::getInstance().clear();
  ImageManager::getInstance().clear();
}

void drawOverlay(float z) {
  C2D_DrawRectSolid(0.0f, 0.0f, z, 400.0f, 240.0f,
                    ScreenManager::colorOverlay());
}

void drawPopupBackground(float x, float y, float w, float h, float z,
                         float radius) {
  drawRoundedRect(x, y, z, w, h, radius, ScreenManager::colorBackground());
}

void drawPopupMenuItem(float x, float y, float w, float h, float z,
                       bool isSelected, u32 selectionColor) {
  if (isSelected) {
    drawRoundedRect(x, y, z, w, h, 6.0f, selectionColor);
  }
}

} // namespace UI
