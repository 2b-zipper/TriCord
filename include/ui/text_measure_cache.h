#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

namespace UI {

class TextMeasureCache {
public:
  static TextMeasureCache &getInstance();

  float measureText(const std::string &text, float scaleX, float scaleY);

  void clear();

  size_t getCacheSize() const;
  size_t getCacheHits() const;
  size_t getCacheMisses() const;

private:
  TextMeasureCache() = default;
  ~TextMeasureCache() = default;
  TextMeasureCache(const TextMeasureCache &) = delete;
  TextMeasureCache &operator=(const TextMeasureCache &) = delete;

  struct CacheKey {
    std::string text;
    float scaleX;
    float scaleY;

    bool operator==(const CacheKey &other) const {
      return text == other.text && scaleX == other.scaleX &&
             scaleY == other.scaleY;
    }
  };

  struct CacheKeyHash {
    size_t operator()(const CacheKey &key) const {
      size_t h1 = std::hash<std::string>{}(key.text);
      size_t h2 = std::hash<float>{}(key.scaleX);
      size_t h3 = std::hash<float>{}(key.scaleY);
      return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
  };

  std::unordered_map<CacheKey, float, CacheKeyHash> cache;
  mutable std::mutex cacheMutex;

  static const size_t MAX_CACHE_SIZE = 1000;

  size_t cacheHits = 0;
  size_t cacheMisses = 0;
};

} // namespace UI
