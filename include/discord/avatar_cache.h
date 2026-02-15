#ifndef AVATAR_CACHE_H
#define AVATAR_CACHE_H

#include <citro2d.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Discord {

struct AvatarInfo {
  C3D_Tex *tex = nullptr;
  std::string url;
  bool loading = false;
};

class AvatarCache {
public:
  static AvatarCache &getInstance();

  void init();
  void shutdown();
  void update();
  void clear();

  C3D_Tex *getAvatar(const std::string &userId, const std::string &avatarHash,
                     const std::string &discriminator);
  C3D_Tex *getGuildIcon(const std::string &guildId,
                        const std::string &iconHash);

  void prefetchAvatar(const std::string &userId, const std::string &avatarHash,
                      const std::string &discriminator);
  void prefetchGuildIcon(const std::string &guildId,
                         const std::string &iconHash);

private:
  AvatarCache() {}
  ~AvatarCache() { clear(); }

  std::map<std::string, AvatarInfo> cache;
  std::recursive_mutex cacheMutex;

  struct PendingAvatar {
    std::string id;
    C3D_Tex *tex = nullptr;
  };
  std::vector<PendingAvatar> pendingAvatars;
};

} // namespace Discord

#endif // AVATAR_CACHE_H
