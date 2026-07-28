#ifndef EMOJI_MANAGER_H
#define EMOJI_MANAGER_H

#include "utils/image_utils.h"
#include "utils/worker_thread.h"
#include <citro2d.h>
#include <condition_variable>
#include <deque>
#include <shared_mutex>
#include <string>
#include <unordered_map>
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

	struct PendingEmoji {
		std::string hex;
		Utils::Image::TiledData tiled;
	};

	void loadEmojiData();
	void loaderWorker();
	void ensureLoaderLocked();
	void freePendingLocked();

	std::unordered_map<std::string, EmojiInfo> emojiCache;
	std::unordered_map<std::string, EmojiInfo> twemojiCache;

	std::deque<std::string> priorityQueue;
	std::deque<std::string> backgroundQueue;
	std::unordered_set<std::string> inQueue;

	std::vector<EmojiCategory> categories;
	std::vector<std::string> allCodepoints;
	bool dataLoaded = false;

	std::shared_mutex cacheMutex;
	uint32_t frameCounter = 0;

	// Uploaded in update(): off-thread C3D_TexInit races the image cache for linear memory.
	std::deque<PendingEmoji> pendingEmoji;

	Utils::WorkerThread loaderThread;
	std::condition_variable_any loaderCv;
	std::condition_variable_any pendingCv;
	bool loaderStarted = false;
	bool stopLoader = false;

	static const int TWEMOJI_DECODE_DIM = 32;

	static const size_t MAX_TWEMOJI_CACHE = 250;
	static const size_t MAX_PENDING_EMOJI = 4;
	static const size_t MAX_UPLOADS_PER_FRAME = 4;
};

} // namespace UI

#endif // EMOJI_MANAGER_H
