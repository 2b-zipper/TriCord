#include "discord/avatar_cache.h"
#include "core/config.h"
#include "network/network_manager.h"

#include "utils/image_utils.h"
#include <malloc.h>

namespace Discord {

AvatarCache &AvatarCache::getInstance() {
	static AvatarCache instance;
	return instance;
}

void AvatarCache::init() {}

void AvatarCache::shutdown() { clear(); }

void AvatarCache::evictOldestLocked() {
	while (cache.size() > MAX_CACHE_ENTRIES) {
		uint32_t oldestFrame = 0xFFFFFFFF;
		auto oldestIt = cache.end();

		for (auto it = cache.begin(); it != cache.end(); ++it) {
			if (it->second.loading) {
				continue;
			}
			if (it->second.lastUsedFrame < oldestFrame) {
				oldestFrame = it->second.lastUsedFrame;
				oldestIt = it;
			}
		}

		if (oldestIt == cache.end()) {
			break;
		}
		if (oldestIt->second.tex) {
			C3D_TexDelete(oldestIt->second.tex);
			free(oldestIt->second.tex);
		}
		cache.erase(oldestIt);
	}
}

void AvatarCache::update() {
	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	frameCounter++;

	size_t uploads = 0;
	while (!pendingAvatars.empty() && uploads < MAX_UPLOADS_PER_FRAME) {
		PendingAvatar pa = pendingAvatars.back();
		pendingAvatars.pop_back();
		uploads++;

		auto it = cache.find(pa.id);
		bool wanted = (it != cache.end() && it->second.loading);

		if (wanted && pa.tiled.pixels) {
			C3D_Tex *tex = (C3D_Tex *)malloc(sizeof(C3D_Tex));
			if (tex && C3D_TexInit(tex, pa.tiled.p2w, pa.tiled.p2h, GPU_RGBA8)) {
				C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
				memcpy(tex->data, pa.tiled.pixels, pa.tiled.vramSize);
				GSPGPU_FlushDataCache(tex->data, pa.tiled.vramSize);
				if (it->second.tex) {
					C3D_TexDelete(it->second.tex);
					free(it->second.tex);
				}
				it->second.tex = tex;
			} else {
				free(tex);
				it->second.attempts = MAX_ATTEMPTS;
			}
			it->second.loading = false;
			it->second.lastUsedFrame = frameCounter;
		}

		if (pa.tiled.pixels) {
			free(pa.tiled.pixels);
			pa.tiled.pixels = nullptr;
		}
	}

	evictOldestLocked();
}

void AvatarCache::freePendingLocked() {
	for (auto &pending : pendingAvatars) {
		if (pending.tiled.pixels) {
			free(pending.tiled.pixels);
			pending.tiled.pixels = nullptr;
		}
	}
	pendingAvatars.clear();
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
	freePendingLocked();
}

C3D_Tex *AvatarCache::getAvatar(const std::string &userId, const std::string &avatarHash,
                                const std::string &discriminator) {
	if (avatarHash.empty() && discriminator.empty()) {
		return nullptr;
	}

	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	auto it = cache.find(userId);
	C3D_Tex *current = (it != cache.end()) ? it->second.tex : nullptr;
	if (it != cache.end()) {
		it->second.lastUsedFrame = frameCounter;
	}
	if (current && it->second.hash == avatarHash) {
		return current;
	}

	prefetchAvatar(userId, avatarHash, discriminator);
	return current;
}

C3D_Tex *AvatarCache::getGuildIcon(const std::string &guildId, const std::string &iconHash) {
	if (iconHash.empty()) {
		return nullptr;
	}

	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	auto it = cache.find(guildId);
	C3D_Tex *current = (it != cache.end()) ? it->second.tex : nullptr;
	if (it != cache.end()) {
		it->second.lastUsedFrame = frameCounter;
	}
	if (current && it->second.hash == iconHash) {
		return current;
	}

	prefetchGuildIcon(guildId, iconHash);
	return current;
}

C3D_Tex *AvatarCache::getChannelIcon(const std::string &channelId, const std::string &iconHash) {
	if (iconHash.empty()) {
		return nullptr;
	}

	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	auto it = cache.find(channelId);
	C3D_Tex *current = (it != cache.end()) ? it->second.tex : nullptr;
	if (it != cache.end()) {
		it->second.lastUsedFrame = frameCounter;
	}
	if (current && it->second.hash == iconHash) {
		return current;
	}

	prefetchChannelIcon(channelId, iconHash);
	return current;
}

bool AvatarCache::shouldFetchLocked(const std::string &key, const std::string &hash) {
	auto it = cache.find(key);
	if (it == cache.end()) {
		return true;
	}
	if (it->second.loading) {
		return false;
	}
	if (it->second.hash != hash) {
		return true;
	}
	if (it->second.tex) {
		return false;
	}
	if (it->second.attempts >= MAX_ATTEMPTS || osGetTime() < it->second.retryAt) {
		return false;
	}
	return true;
}

void AvatarCache::startFetchLocked(const std::string &key, const std::string &url, const std::string &hash,
                                   float cornerRatio) {
	int attempts = 0;
	C3D_Tex *current = nullptr;
	auto existing = cache.find(key);
	if (existing != cache.end()) {
		current = existing->second.tex;
		if (existing->second.hash == hash) {
			attempts = existing->second.attempts;
		}
	}

	AvatarInfo info;
	info.url = url;
	info.hash = hash;
	info.loading = true;
	info.attempts = attempts;
	info.tex = current;
	info.lastUsedFrame = frameCounter;
	cache[key] = info;

	Network::NetworkManager::getInstance().enqueue(
	    url, "GET", "", Network::RequestPriority::BACKGROUND,
	    [this, key, cornerRatio](const Network::HttpResponse &resp) {
		    if (resp.statusCode == 200 && !resp.body.empty()) {
			    Utils::Image::TiledData tiled = Utils::Image::decodeToTiled(
			        (const unsigned char *)resp.body.data(), resp.body.size(), Utils::Image::MAX_REMOTE_DIM,
			        Utils::Image::MAX_REMOTE_DIM, true, cornerRatio);
			    if (tiled.pixels) {
				    std::lock_guard<std::recursive_mutex> lock(this->cacheMutex);
				    PendingAvatar pa;
				    pa.id = key;
				    pa.tiled = tiled;
				    this->pendingAvatars.push_back(pa);
				    return;
			    }
		    }

		    std::lock_guard<std::recursive_mutex> lock(this->cacheMutex);
		    auto it = this->cache.find(key);
		    if (it != this->cache.end()) {
			    it->second.loading = false;
			    it->second.attempts++;
			    it->second.retryAt = osGetTime() + (u64)3000 * it->second.attempts;
		    }
	    });
}

void AvatarCache::prefetchAvatar(const std::string &userId, const std::string &avatarHash,
                                 const std::string &discriminator) {
	if ((avatarHash.empty() && discriminator.empty()) || !Config::getInstance().isShowAvatarsEnabled()) {
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	if (!shouldFetchLocked(userId, avatarHash)) {
		return;
	}

	std::string url;
	if (!avatarHash.empty()) {
		url = "https://cdn.discordapp.com/avatars/" + userId + "/" + avatarHash +
		      ".png?size=" + std::to_string(AVATAR_SIZE);
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
		url = "https://cdn.discordapp.com/embed/avatars/" + std::to_string(index) +
		      ".png?size=" + std::to_string(AVATAR_SIZE);
	}

	startFetchLocked(userId, url, avatarHash, 0.5f);
}

void AvatarCache::prefetchGuildIcon(const std::string &guildId, const std::string &iconHash) {
	if (iconHash.empty() || !Config::getInstance().isShowServerIconsEnabled()) {
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	if (!shouldFetchLocked(guildId, iconHash)) {
		return;
	}

	startFetchLocked(guildId,
	                 "https://cdn.discordapp.com/icons/" + guildId + "/" + iconHash +
	                     ".png?size=" + std::to_string(GUILD_ICON_SIZE),
	                 iconHash, GUILD_ICON_CORNER_RATIO);
}

void AvatarCache::prefetchChannelIcon(const std::string &channelId, const std::string &iconHash) {
	if (iconHash.empty() || !Config::getInstance().isShowServerIconsEnabled()) {
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	if (!shouldFetchLocked(channelId, iconHash)) {
		return;
	}

	startFetchLocked(channelId,
	                 "https://cdn.discordapp.com/channel-icons/" + channelId + "/" + iconHash +
	                     ".png?size=" + std::to_string(AVATAR_SIZE),
	                 iconHash, 0.5f);
}

} // namespace Discord
