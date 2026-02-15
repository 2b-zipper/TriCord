#include "discord/avatar_cache.h"
#include "network/network_manager.h"
#include "ui/emoji_manager.h"

#include "utils/image_utils.h"
#include <malloc.h>

namespace Discord {

AvatarCache &AvatarCache::getInstance() {
  static AvatarCache instance;
  return instance;
}

void AvatarCache::init() {}

void AvatarCache::shutdown() { clear(); }

void AvatarCache::update() {
  std::lock_guard<std::recursive_mutex> lock(cacheMutex);
  while (!pendingAvatars.empty()) {
    PendingAvatar pa = pendingAvatars.back();
    pendingAvatars.pop_back();

    auto it = cache.find(pa.id);
    if (it != cache.end() && it->second.loading) {
      it->second.tex = pa.tex;
      it->second.loading = false;
    } else if (pa.tex) {
      C3D_TexDelete(pa.tex);
      free(pa.tex);
    }
  }
}

void AvatarCache::clear() {
  std::lock_guard<std::recursive_mutex> lock(cacheMutex);
  for (auto &pair : cache) {
    if (pair.second.tex) {
      C3D_TexDelete(pair.second.tex);
      free(pair.second.tex);
    }
  }
  cache.clear();
  pendingAvatars.clear();
}

C3D_Tex *AvatarCache::getAvatar(const std::string &userId,
                                const std::string &avatarHash,
                                const std::string &discriminator) {
  if (avatarHash.empty() && discriminator.empty())
    return nullptr;

  std::lock_guard<std::recursive_mutex> lock(cacheMutex);
  auto it = cache.find(userId);
  if (it != cache.end()) {
    if (it->second.tex)
      return it->second.tex;
    return nullptr;
  }

  prefetchAvatar(userId, avatarHash, discriminator);
  return nullptr;
}

C3D_Tex *AvatarCache::getGuildIcon(const std::string &guildId,
                                   const std::string &iconHash) {
  if (iconHash.empty())
    return nullptr;

  std::lock_guard<std::recursive_mutex> lock(cacheMutex);
  auto it = cache.find(guildId);
  if (it != cache.end()) {
    if (it->second.tex)
      return it->second.tex;
    return nullptr;
  }

  prefetchGuildIcon(guildId, iconHash);
  return nullptr;
}

void AvatarCache::prefetchAvatar(const std::string &userId,
                                 const std::string &avatarHash,
                                 const std::string &discriminator) {
  if (avatarHash.empty() && discriminator.empty())
    return;

  std::lock_guard<std::recursive_mutex> lock(cacheMutex);
  if (cache.count(userId))
    return;

  AvatarInfo info;
  if (!avatarHash.empty()) {
    info.url = "https://cdn.discordapp.com/avatars/" + userId + "/" +
               avatarHash + ".png?size=64";
  } else {

    int index = 0;
    if (!discriminator.empty() && discriminator != "0") {
      index = std::atoi(discriminator.c_str()) % 5;
    } else {
      unsigned long long uid = 0;
      for (char c : userId) {
        if (c >= '0' && c <= '9') {
          uid = uid * 10 + (c - '0');
        }
      }
      index = (uid >> 22) % 6;
    }
    info.url = "https://cdn.discordapp.com/embed/avatars/" +
               std::to_string(index) + ".png";
  }

  info.loading = true;
  cache[userId] = info;

  Network::NetworkManager::getInstance().enqueue(
      info.url, "GET", "", Network::RequestPriority::BACKGROUND,
      [this, userId](const Network::HttpResponse &resp) {
        if (resp.statusCode == 200 && !resp.body.empty()) {
          C3D_Tex *tex = Utils::Image::loadTextureFromMemory(
              (const unsigned char *)resp.body.data(), resp.body.size());

          std::lock_guard<std::recursive_mutex> lock(this->cacheMutex);
          PendingAvatar pa;
          pa.id = userId;
          pa.tex = tex;
          this->pendingAvatars.push_back(pa);
        } else {
          std::lock_guard<std::recursive_mutex> lock(this->cacheMutex);
          auto it = this->cache.find(userId);
          if (it != this->cache.end())
            it->second.loading = false;
        }
      });
}

void AvatarCache::prefetchGuildIcon(const std::string &guildId,
                                    const std::string &iconHash) {
  if (iconHash.empty())
    return;

  std::lock_guard<std::recursive_mutex> lock(cacheMutex);
  if (cache.count(guildId))
    return;

  AvatarInfo info;
  info.url = "https://cdn.discordapp.com/icons/" + guildId + "/" + iconHash +
             ".png?size=64";
  info.loading = true;
  cache[guildId] = info;

  Network::NetworkManager::getInstance().enqueue(
      info.url, "GET", "", Network::RequestPriority::BACKGROUND,
      [this, guildId](const Network::HttpResponse &resp) {
        if (resp.statusCode == 200 && !resp.body.empty()) {
          C3D_Tex *tex = Utils::Image::loadTextureFromMemory(
              (const unsigned char *)resp.body.data(), resp.body.size());

          std::lock_guard<std::recursive_mutex> lock(this->cacheMutex);
          PendingAvatar pa;
          pa.id = guildId;
          pa.tex = tex;
          this->pendingAvatars.push_back(pa);
        } else {
          std::lock_guard<std::recursive_mutex> lock(this->cacheMutex);
          auto it = this->cache.find(guildId);
          if (it != this->cache.end())
            it->second.loading = false;
        }
      });
}

} // namespace Discord
