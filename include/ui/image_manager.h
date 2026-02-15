#ifndef IMAGE_MANAGER_H
#define IMAGE_MANAGER_H

#include "network/network_manager.h"
#include <atomic>
#include <citro2d.h>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace UI {

class ImageManager {
public:
  static ImageManager &getInstance();

  void init();
  void shutdown();

  struct ImageInfo {
    C3D_Tex *tex = nullptr;
    int originalW = 0;
    int originalH = 0;
    bool failed = false;
  };

  C3D_Tex *getImage(const std::string &url);
  ImageInfo getImageInfo(const std::string &url);

  C3D_Tex *getLocalImage(const std::string &path, bool noResize = false);

  void prefetch(
      const std::string &url, int origW = 0, int origH = 0,
      Network::RequestPriority priority = Network::RequestPriority::BACKGROUND);

  void update();

  void clear();

  void clearFailed(const std::string &url);

private:
  ImageManager() = default;
  ~ImageManager();

  struct PendingTexture {
    std::string url;
    C3D_Tex *tex = nullptr;
    int width = 0;
    int height = 0;
    bool success = false;
  };

  std::map<std::string, ImageInfo> textureCache;
  std::set<std::string> fetchingUrls;
  std::deque<PendingTexture> pendingTextures;
  std::mutex cacheMutex;
  std::atomic<int> currentSessionId{0};
};

} // namespace UI

#endif // IMAGE_MANAGER_H
