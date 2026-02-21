#include "ui/emoji_manager.h"
#include "log.h"
#include "network/network_manager.h"
#include "utils/image_utils.h"
#include <cstdio>
#include <malloc.h>

namespace UI {

EmojiManager &EmojiManager::getInstance() {
  static EmojiManager instance;
  return instance;
}

void EmojiManager::init() {}

void EmojiManager::shutdown() {
  std::unique_lock<std::shared_mutex> lock(cacheMutex);
  for (auto &pair : emojiCache) {
    if (pair.second.tex) {
      C3D_TexDelete(pair.second.tex);
      free(pair.second.tex);
    }
  }
  for (auto &pair : twemojiCache) {
    if (pair.second.tex) {
      C3D_TexDelete(pair.second.tex);
      free(pair.second.tex);
    }
  }
  emojiCache.clear();
  twemojiCache.clear();
}

EmojiManager::~EmojiManager() { shutdown(); }

void EmojiManager::update() {}

EmojiManager::EmojiInfo EmojiManager::getEmojiInfo(const std::string &emojiId) {
  std::shared_lock<std::shared_mutex> lock(cacheMutex);
  auto it = emojiCache.find(emojiId);
  if (it != emojiCache.end()) {
    return it->second;
  }

  return EmojiInfo();
}

void EmojiManager::prefetchEmoji(const std::string &emojiId) {
  if (emojiId.empty())
    return;

  {
    std::unique_lock<std::shared_mutex> lock(cacheMutex);
    if (emojiCache.count(emojiId))
      return;

    EmojiInfo placeholder;
    placeholder.tex = nullptr;
    placeholder.originalW = 0;
    placeholder.originalH = 0;
    emojiCache[emojiId] = placeholder;
  }

  std::string url =
      "https://media.discordapp.net/emojis/" + emojiId + ".png?size=32";

  Network::NetworkManager::getInstance().enqueue(
      url, "GET", "", Network::RequestPriority::INTERACTIVE,
      [this, emojiId](const Network::HttpResponse &resp) {
        if (resp.statusCode == 200 && !resp.body.empty()) {
          int w, h;
          C3D_Tex *tex = Utils::Image::loadTextureFromMemory(
              (const unsigned char *)resp.body.data(), resp.body.size(), w, h);
          if (tex) {
            std::unique_lock<std::shared_mutex> lock(cacheMutex);
            EmojiInfo info;
            info.tex = tex;
            info.originalW = w;
            info.originalH = h;
            emojiCache[emojiId] = info;
          }
        }
      });
}

void EmojiManager::prefetchEmojisFromText(const std::string &text) {
  size_t cursor = 0;
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
              prefetchEmoji(id);
              cursor = closeBracket + 1;
              continue;
            }
          }
        }
      }
    }
    cursor++;
  }
}

EmojiManager::EmojiInfo
EmojiManager::getTwemojiInfo(const std::string &codepointHex) {
  {
    std::shared_lock<std::shared_mutex> lock(cacheMutex);
    auto it = twemojiCache.find(codepointHex);
    if (it != twemojiCache.end()) {
      return it->second;
    }
  }

  auto getPath = [](const std::string &h) {
    return "romfs:/twemoji17/" + h + ".png";
  };

  std::string hex = codepointHex;
  std::string path = getPath(hex);
  FILE *f = fopen(path.c_str(), "rb");

  if (!f) {
    std::string strippedHex = hex;
    size_t pos = 0;
    while ((pos = strippedHex.find("-fe0f")) != std::string::npos) {
      strippedHex.erase(pos, 5);
    }
    if (strippedHex != hex) {
      hex = strippedHex;
      path = getPath(hex);
      f = fopen(path.c_str(), "rb");
    }
  }

  if (f) {
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<unsigned char> buffer(size);
    fread(buffer.data(), 1, size, f);
    fclose(f);

    int w, h;
    C3D_Tex *tex =
        Utils::Image::loadTextureFromMemory(buffer.data(), size, w, h);
    if (tex) {
      EmojiInfo info;
      info.tex = tex;
      info.originalW = w;
      info.originalH = h;

      {
        std::unique_lock<std::shared_mutex> lock(cacheMutex);
        twemojiCache[codepointHex] = info;
      }
      return info;
    }
  }

  return EmojiInfo();
}

} // namespace UI
