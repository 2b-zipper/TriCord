#ifndef AVATAR_CACHE_H
#define AVATAR_CACHE_H

#include "utils/image_utils.h"
#include <citro2d.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Discord {

struct AvatarInfo {
	C3D_Tex *tex = nullptr;
	std::string url;
	std::string hash;
	bool loading = false;
	int attempts = 0;
	u64 retryAt = 0;
	uint32_t lastUsedFrame = 0;
};

class AvatarCache {
  public:
	static AvatarCache &getInstance();

	void init();
	void shutdown();
	void update();
	void clear();

	C3D_Tex *getAvatar(const std::string &userId, const std::string &avatarHash, const std::string &discriminator);
	C3D_Tex *getGuildIcon(const std::string &guildId, const std::string &iconHash);
	C3D_Tex *getChannelIcon(const std::string &channelId, const std::string &iconHash);

	void prefetchAvatar(const std::string &userId, const std::string &avatarHash, const std::string &discriminator);
	void prefetchGuildIcon(const std::string &guildId, const std::string &iconHash);
	void prefetchChannelIcon(const std::string &channelId, const std::string &iconHash);

  private:
	AvatarCache() {}
	~AvatarCache() { clear(); }

	std::map<std::string, AvatarInfo> cache;
	std::recursive_mutex cacheMutex;

	struct PendingAvatar {
		std::string id;
		Utils::Image::TiledData tiled;
	};
	std::vector<PendingAvatar> pendingAvatars;

	static constexpr int AVATAR_SIZE = 64;
	static constexpr int GUILD_ICON_SIZE = 64;
	static constexpr size_t MAX_UPLOADS_PER_FRAME = 4;
	static constexpr int MAX_ATTEMPTS = 3;
	static constexpr size_t MAX_CACHE_ENTRIES = 128;

	uint32_t frameCounter = 0;
	void evictOldestLocked();

	bool shouldFetchLocked(const std::string &key, const std::string &hash);
	static constexpr float GUILD_ICON_CORNER_RATIO = 1.0f / 3.0f;
	void startFetchLocked(const std::string &key, const std::string &url, const std::string &hash,
	                      float cornerRatio = 0.0f);
	void freePendingLocked();
};

} // namespace Discord

#endif // AVATAR_CACHE_H
