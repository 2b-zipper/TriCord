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

ImageManager::~ImageManager() { clear(); }

void ImageManager::init() {}

void ImageManager::shutdown() { clear(); }

void ImageManager::clear() {
  std::lock_guard<std::mutex> lock(cacheMutex);
  for (auto &pair : textureCache) {
    if (pair.second.tex) {
      C3D_TexDelete(pair.second.tex);
      free(pair.second.tex);
    }
  }
  textureCache.clear();
  fetchingUrls.clear();
  pendingTextures.clear();
  currentSessionId++;
}

void ImageManager::clearFailed(const std::string &url) {
  std::lock_guard<std::mutex> lock(cacheMutex);
  auto it = textureCache.find(url);
  if (it != textureCache.end() && it->second.failed) {
    textureCache.erase(it);
  }
  fetchingUrls.erase(url);
}

C3D_Tex *ImageManager::getImage(const std::string &url) {
  if (url.empty())
    return nullptr;

  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (textureCache.find(url) != textureCache.end()) {
      return textureCache[url].tex;
    }
  }

  prefetch(url);
  return nullptr;
}

ImageManager::ImageInfo ImageManager::getImageInfo(const std::string &url) {
  std::lock_guard<std::mutex> lock(cacheMutex);
  if (textureCache.find(url) != textureCache.end()) {
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
    if (textureCache.find(url) != textureCache.end())
      return;
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

    if (optimizedUrl.find("?") == std::string::npos) {
      optimizedUrl += "?width=" + std::to_string(targetW) +
                      "&height=" + std::to_string(targetH) + "&format=jpeg";
    } else {
      if (optimizedUrl.find("width=") == std::string::npos) {
        optimizedUrl += "&width=" + std::to_string(targetW) +
                        "&height=" + std::to_string(targetH) + "&format=jpeg";
      }
    }
  }

  int sessionId = currentSessionId;
  Network::NetworkManager::getInstance().enqueue(
      optimizedUrl, "GET", "", priority,
      [this, url, sessionId](const Network::HttpResponse &resp) {
        if (currentSessionId != sessionId)
          return;

        if (resp.success && resp.statusCode == 200 && !resp.body.empty()) {
          int w = 0, h = 0;
          C3D_Tex *tex = Utils::Image::loadTextureFromMemory(
              (const unsigned char *)resp.body.data(), resp.body.size(), w, h);

          PendingTexture pending;
          pending.url = url;
          pending.tex = tex;
          pending.success = (tex != nullptr);
          pending.width = w;
          pending.height = h;

          std::lock_guard<std::mutex> lock(cacheMutex);
          pendingTextures.push_back(pending);
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

void ImageManager::update() {
  PendingTexture p;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (pendingTextures.empty())
      return;
    p = std::move(pendingTextures.front());
    pendingTextures.pop_front();
  }

  C3D_Tex *tex = p.tex;
  int width = p.width;
  int height = p.height;

  std::lock_guard<std::mutex> lock(cacheMutex);
  fetchingUrls.erase(p.url);

  if (tex) {
    ImageInfo info;
    info.tex = tex;
    info.originalW = width;
    info.originalH = height;
    info.failed = false;
    textureCache[p.url] = info;
  } else {
    ImageInfo info;
    info.failed = true;
    textureCache[p.url] = info;
  }
}

} // namespace UI
