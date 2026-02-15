#ifndef EMOJI_MANAGER_H
#define EMOJI_MANAGER_H

#include <citro2d.h>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace UI {

class EmojiManager {
public:
  static EmojiManager &getInstance();

  void init();
  void shutdown();
  void update();

  struct EmojiInfo {
    C3D_Tex *tex = nullptr;
    int originalW = 0;
    int originalH = 0;
  };

  EmojiInfo getEmojiInfo(const std::string &emojiId);
  EmojiInfo getTwemojiInfo(const std::string &codepointHex);

  void prefetchEmoji(const std::string &emojiId);

private:
  EmojiManager() = default;
  ~EmojiManager();

  std::map<std::string, EmojiInfo> emojiCache;
  std::map<std::string, EmojiInfo> twemojiCache;
  std::shared_mutex cacheMutex;
};

} // namespace UI

#endif // EMOJI_MANAGER_H
