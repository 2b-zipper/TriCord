#ifndef EMOJI_MANAGER_H
#define EMOJI_MANAGER_H

#include <citro2d.h>
#include <deque>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace UI {

struct EmojiCategory {
	std::vector<size_t> emojiIndices;
};

struct EmojiDataEntry {
	char hex[56];
	int32_t order;
	int32_t category;
};

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
		uint32_t lastUsedFrame = 0;
		bool isLoading = false;
	};

	EmojiInfo getEmojiInfo(const std::string &emojiId);
	EmojiInfo getTwemojiInfo(const std::string &codepointHex);

	void prefetchEmoji(const std::string &emojiId);
	void prefetchEmojisFromText(const std::string &text);

	const std::vector<EmojiCategory> &getCategories();
	const std::vector<std::string> &getCodepoints();
	void onCategoryChanged(const std::unordered_set<std::string> &keep = {});

  private:
	EmojiManager() = default;
	~EmojiManager();

	void loadEmojiData();

	std::map<std::string, EmojiInfo> emojiCache;
	std::map<std::string, EmojiInfo> twemojiCache;

	std::deque<std::string> priorityQueue;
	std::deque<std::string> backgroundQueue;
	std::unordered_set<std::string> inQueue;

	std::vector<EmojiCategory> categories;
	std::vector<std::string> allCodepoints;
	bool dataLoaded = false;

	std::shared_mutex cacheMutex;
	uint32_t frameCounter = 0;

	static const size_t MAX_TWEMOJI_CACHE = 250;
};

} // namespace UI

#endif // EMOJI_MANAGER_H
