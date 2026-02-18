#ifndef IMAGE_MANAGER_H
#define IMAGE_MANAGER_H

#include "network/network_manager.h"
#include "utils/image_utils.h"
#include <atomic>
#include <citro2d.h>
#include <deque>
#include <list>
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
  void clearRemote();

private:
  ImageManager() = default;
  ~ImageManager();

  struct PendingTexture {
    std::string url;
    C3D_Tex *tex = nullptr;
    Utils::Image::TiledData tiled;
    int width = 0;
    int height = 0;
    bool success = false;
  };

  struct DecodeRequest {
    std::string url;
    std::string body;
    int maxWidth = 512;
    int maxHeight = 512;
    int sessionId = 0;
    Network::RequestPriority priority;
  };

  std::map<std::string, ImageInfo> textureCache;
  std::list<std::string> lruList;
  std::set<std::string> fetchingUrls;
  std::deque<PendingTexture> pendingTextures;
  std::deque<DecodeRequest> decodeQueue;

  std::mutex cacheMutex;
  std::mutex decodeMutex;
  std::condition_variable decodeCv;
  std::thread decoderThread;
  std::atomic<bool> stopDecoder{false};

  std::atomic<int> currentSessionId{0};

  static constexpr size_t MAX_TEXTURES = 15;
  void touchImage(const std::string &url);
  void evictOldest();
  void decoderWorker();
};

} // namespace UI

#endif // IMAGE_MANAGER_H
