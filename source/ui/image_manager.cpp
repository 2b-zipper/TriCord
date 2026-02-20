#include "ui/image_manager.h"
#include "log.h"
#include "network/network_manager.h"
#include "utils/image_utils.h"

#include <cstring>

namespace UI {

ImageManager &ImageManager::getInstance() {
  static ImageManager instance;
  return instance;
}

ImageManager::~ImageManager() {
  shutdown();
  clear();
}

void ImageManager::init() {
  if (!decoderThread.joinable()) {
    stopDecoder = false;
    decoderThread = std::thread(&ImageManager::decoderWorker, this);
  }
}

void ImageManager::shutdown() {
  {
    std::lock_guard<std::mutex> lock(decodeMutex);
    stopDecoder = true;
  }
  decodeCv.notify_all();
  if (decoderThread.joinable())
    decoderThread.join();

  clear();
}

void ImageManager::clear() {
  std::lock_guard<std::mutex> lock(cacheMutex);
  for (auto &pair : textureCache) {
    if (pair.second.tex) {
      C3D_TexDelete(pair.second.tex);
      free(pair.second.tex);
    }
  }
  textureCache.clear();
  lruList.clear();
  fetchingUrls.clear();
  pendingTextures.clear();
  {
    std::lock_guard<std::mutex> lock(decodeMutex);
    decodeQueue.clear();
  }
  currentSessionId++;
}

void ImageManager::touchImage(const std::string &url) {
  if (url.find("http") != 0)
    return; // Don't LRU local images

  lruList.remove(url);
  lruList.push_front(url);
}

void ImageManager::evictOldest() {
  if (lruList.empty())
    return;

  std::string url = lruList.back();
  lruList.pop_back();

  auto it = textureCache.find(url);
  if (it != textureCache.end()) {
    if (it->second.tex) {
      C3D_TexDelete(it->second.tex);
      free(it->second.tex);
    }
    textureCache.erase(it);
  }
}

void ImageManager::clearFailed(const std::string &url) {
  std::lock_guard<std::mutex> lock(cacheMutex);
  auto it = textureCache.find(url);
  if (it != textureCache.end() && it->second.failed) {
    textureCache.erase(it);
    lruList.remove(url);
  }
  fetchingUrls.erase(url);
}

void ImageManager::clearRemote() {
  std::lock_guard<std::mutex> lock(cacheMutex);
  for (auto it = textureCache.begin(); it != textureCache.end();) {
    if (it->first.find("http") == 0) {
      if (it->second.tex) {
        C3D_TexDelete(it->second.tex);
        free(it->second.tex);
      }
      it = textureCache.erase(it);
    } else {
      ++it;
    }
  }
  lruList.clear();
  fetchingUrls.clear();
  pendingTextures.clear();
  {
    std::lock_guard<std::mutex> lock2(decodeMutex);
    decodeQueue.clear();
  }
  currentSessionId++;
}

C3D_Tex *ImageManager::getImage(const std::string &url) {
  if (url.empty())
    return nullptr;

  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (textureCache.find(url) != textureCache.end()) {
      touchImage(url);
      return textureCache[url].tex;
    }
  }

  prefetch(url);
  return nullptr;
}

ImageManager::ImageInfo ImageManager::getImageInfo(const std::string &url) {
  std::lock_guard<std::mutex> lock(cacheMutex);
  if (textureCache.find(url) != textureCache.end()) {
    touchImage(url);
    return textureCache[url];
  }
  return ImageInfo();
}

C3D_Tex *ImageManager::getLocalImage(const std::string &path, bool noResize) {
  if (path.empty())
    return nullptr;

  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (textureCache.find(path) != textureCache.end()) {
      return textureCache[path].tex;
    }
  }

  Logger::log("[Image] Loading local: %s", path.c_str());

  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    Logger::log("[Image] Failed to open local file: %s", path.c_str());
    return nullptr;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size <= 0) {
    fclose(f);
    return nullptr;
  }

  std::vector<unsigned char> data(size);
  fread(data.data(), 1, size, f);
  fclose(f);

  int outW = 0, outH = 0;

  C3D_Tex *tex = Utils::Image::loadTextureFromMemory(
      (const unsigned char *)data.data(), size, outW, outH, noResize);
  if (tex) {
    ImageInfo info;
    info.tex = tex;
    info.originalW = outW;
    info.originalH = outH;
    std::lock_guard<std::mutex> lock(cacheMutex);
    textureCache[path] = info;
    return tex;
  }

  return nullptr;
}

void ImageManager::prefetch(const std::string &url, int origW, int origH,
                            Network::RequestPriority priority) {
  if (url.empty())
    return;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = textureCache.find(url);
    if (it != textureCache.end()) {
      if (!it->second.failed)
        return;
      textureCache.erase(it);
    }
    if (fetchingUrls.find(url) != fetchingUrls.end())
      return;

    fetchingUrls.insert(url);
  }

  std::string optimizedUrl = url;

  if (optimizedUrl.find("cdn.discordapp.com") != std::string::npos) {
    size_t pos = optimizedUrl.find("cdn.discordapp.com");
    optimizedUrl.replace(pos, 18, "media.discordapp.net");
  }

  if (optimizedUrl.find("media.discordapp.net") != std::string::npos ||
      optimizedUrl.find("images-ext-") != std::string::npos) {
    int targetW = 512;
    int targetH = 512;
    if (origW > 0 && origH > 0) {
      if (origW > targetW || origH > targetH) {
        if (origW > origH) {
          targetH = (targetW * origH) / origW;
        } else {
          targetW = (targetH * origW) / origH;
        }
      } else {
        targetW = origW;
        targetH = origH;
      }
    }

    bool hasParams = (optimizedUrl.find("?") != std::string::npos);
    std::string separator = hasParams ? "&" : "?";

    if (optimizedUrl.find("avatars/") != std::string::npos ||
        optimizedUrl.find("icons/") != std::string::npos ||
        optimizedUrl.find("banners/") != std::string::npos ||
        optimizedUrl.find("splashes/") != std::string::npos ||
        optimizedUrl.find("app-icons/") != std::string::npos) {
      int p2size = 64;
      if (std::max(targetW, targetH) > 64)
        p2size = 128;
      if (std::max(targetW, targetH) > 128)
        p2size = 256;

      if (optimizedUrl.find("size=") == std::string::npos) {
        optimizedUrl += separator + "size=" + std::to_string(p2size);
        separator = "&";
      }
      if (optimizedUrl.find("format=") == std::string::npos) {
        optimizedUrl += separator + "format=jpeg";
      }
    } else {
      if (optimizedUrl.find("width=") == std::string::npos) {
        optimizedUrl += separator + "width=" + std::to_string(targetW) +
                        "&height=" + std::to_string(targetH);
        separator = "&";
      }
      if (optimizedUrl.find("format=") == std::string::npos) {
        optimizedUrl += separator + "format=jpeg";
      }
    }
  }

  int sessionId = currentSessionId;
  Network::NetworkManager::getInstance().enqueue(
      optimizedUrl, "GET", "", priority,
      [this, url, sessionId, priority](const Network::HttpResponse &resp) {
        if (currentSessionId != sessionId)
          return;

        if (resp.success && resp.statusCode == 200 && !resp.body.empty()) {
          DecodeRequest req;
          req.url = url;
          req.body = std::move(resp.body);
          req.sessionId = sessionId;
          req.priority = priority;

          std::lock_guard<std::mutex> lock(decodeMutex);
          decodeQueue.push_back(std::move(req));
          decodeCv.notify_one();
        } else {
          Logger::log("[Image] Fetch failed for %s. Status: %d, Body size: %zu",
                      url.c_str(), resp.statusCode, resp.body.size());
          std::lock_guard<std::mutex> lock(cacheMutex);
          ImageInfo info;
          info.failed = true;
          textureCache[url] = info;
          fetchingUrls.erase(url);
        }
      });
}

void ImageManager::decoderWorker() {
  while (true) {
    DecodeRequest req;
    {
      std::unique_lock<std::mutex> lock(decodeMutex);
      decodeCv.wait(lock,
                    [this] { return stopDecoder || !decodeQueue.empty(); });
      if (stopDecoder)
        return;
      req = std::move(decodeQueue.front());
      decodeQueue.pop_front();
    }

    if (currentSessionId != req.sessionId)
      continue;

    Utils::Image::TiledData tiled = Utils::Image::decodeToTiled(
        (const unsigned char *)req.body.data(), req.body.size());

    PendingTexture pending;
    pending.url = req.url;
    pending.tiled = tiled;
    pending.success = (tiled.pixels != nullptr);
    pending.width = tiled.w;
    pending.height = tiled.h;

    std::lock_guard<std::mutex> lock(cacheMutex);
    pendingTextures.push_back(pending);
  }
}

void ImageManager::update() {
  PendingTexture p;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (pendingTextures.empty())
      return;
    p = std::move(pendingTextures.front());
    pendingTextures.pop_front();
  }

  fetchingUrls.erase(p.url);

  if (p.success) {
    if (p.tiled.pixels) {
      while (lruList.size() >= MAX_TEXTURES) {
        evictOldest();
      }

      C3D_Tex *tex = (C3D_Tex *)malloc(sizeof(C3D_Tex));
      if (C3D_TexInit(tex, p.tiled.p2w, p.tiled.p2h, GPU_RGBA8)) {
        C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
        memcpy(tex->data, p.tiled.pixels, p.tiled.vramSize);
        GSPGPU_FlushDataCache(tex->data, p.tiled.vramSize);

        ImageInfo info;
        info.tex = tex;
        info.originalW = p.width;
        info.originalH = p.height;
        info.failed = false;

        std::lock_guard<std::mutex> lock(cacheMutex);
        textureCache[p.url] = info;
        touchImage(p.url);
        generation++;
      } else {
        free(tex);
        std::lock_guard<std::mutex> lock(cacheMutex);
        ImageInfo info;
        info.failed = true;
        textureCache[p.url] = info;
        generation++;
      }
      free(p.tiled.pixels);
    }
  } else {
    std::lock_guard<std::mutex> lock(cacheMutex);
    ImageInfo info;
    info.failed = true;
    textureCache[p.url] = info;
    generation++;
  }
}

} // namespace UI
