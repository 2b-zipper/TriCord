#include "ui/text_measure_cache.h"
#include "ui/screen_manager.h"
#include <algorithm>

namespace UI {

TextMeasureCache &TextMeasureCache::getInstance() {
  static TextMeasureCache instance;
  return instance;
}

float TextMeasureCache::measureText(const std::string &text, float scaleX,
                                    float scaleY) {
  CacheKey key{text, scaleX, scaleY};

  {
    std::lock_guard<std::mutex> lock(cacheMutex);

    auto it = cache.find(key);
    if (it != cache.end()) {
      cacheHits++;
      return it->second;
    }

    cacheMisses++;
  }

  float width = UI::measureTextDirect(text, scaleX, scaleY);

  {
    std::lock_guard<std::mutex> lock(cacheMutex);

    cache[key] = width;

    // Simple LRU: if cache is too large, clear it
    // (A proper LRU would track access order, but this is simpler)
    if (cache.size() > MAX_CACHE_SIZE) {
      cache.clear();
      cacheHits = 0;
      cacheMisses = 0;
    }
  }

  return width;
}

void TextMeasureCache::clear() {
  std::lock_guard<std::mutex> lock(cacheMutex);
  cache.clear();
  cacheHits = 0;
  cacheMisses = 0;
}

size_t TextMeasureCache::getCacheSize() const {
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cache.size();
}

size_t TextMeasureCache::getCacheHits() const {
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cacheHits;
}

size_t TextMeasureCache::getCacheMisses() const {
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cacheMisses;
}

} // namespace UI
