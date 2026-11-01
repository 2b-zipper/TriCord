#ifndef IMAGE_MANAGER_H
#define IMAGE_MANAGER_H

#include "network/network_manager.h"
#include "utils/image_utils.h"
#include "utils/worker_thread.h"
#include <atomic>
#include <citro2d.h>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace UI {

class ImageManager {
  public:
	static ImageManager &getInstance();

	void init();
	void shutdown();

	struct Animation;

	struct ImageInfo {
		C3D_Tex *tex = nullptr;
		int originalW = 0;
		int originalH = 0;
		size_t vramSize = 0;
		bool failed = false;
		std::shared_ptr<Animation> animation;
	};

	C3D_Tex *getImage(const std::string &url);
	ImageInfo getImageInfo(std::string_view url);

	C3D_Tex *getLocalImage(std::string_view path, bool noResize = false);

	void prefetch(const std::string &url, int origW = 0, int origH = 0,
	              Network::RequestPriority priority = Network::RequestPriority::BACKGROUND);

	void update();

	// A frame only advances once the previous one reached the screen, which is
	// what stops a GIF that has scrolled away.
	void markDrawn(const ImageInfo &info);

	void runOnDecoder(std::function<void()> job);

	void clear();
	void clearFailed(const std::string &url);
	void clearRemote();
	uint32_t getGeneration() const { return generation; }

	size_t getCacheBytes();
	size_t getCacheCount();
	static size_t getCacheBudget();

  private:
	ImageManager() = default;
	~ImageManager();

	struct PendingTexture {
		std::string url;
		C3D_Tex *tex = nullptr;
		Utils::Image::TiledData tiled;
		std::shared_ptr<Animation> animation;
		int width = 0;
		int height = 0;
		bool success = false;
	};

	struct DecodeRequest {
		std::string url;
		std::string body;
		int sessionId = 0;
		Network::RequestPriority priority;
	};

	std::map<std::string, ImageInfo, std::less<>> textureCache;
	std::list<std::string> lruList;
	std::set<std::string> fetchingUrls;
	std::set<std::string> stillFallback;
	std::deque<PendingTexture> pendingTextures;
	std::deque<DecodeRequest> decodeQueue;
	// clear() must not drop these.
	std::deque<std::function<void()>> jobQueue;

	std::mutex cacheMutex;
	std::mutex decodeMutex;
	std::condition_variable decodeCv;
	std::condition_variable pendingCv;
	Utils::WorkerThread decoderThread;
	std::atomic<bool> stopDecoder{false};

	std::vector<std::shared_ptr<Animation>> animations;
	std::mutex animationMutex;
	std::condition_variable animationCv;
	Utils::WorkerThread animationThread;
	bool stopAnimation = false;

	std::atomic<int> currentSessionId{0};
	std::atomic<uint32_t> generation{0};

	static constexpr size_t MIN_CACHE_ENTRIES = 8;
	static constexpr size_t MAX_PENDING_TEXTURES = 4;
	size_t currentCacheBytes = 0;
	void touchImage(const std::string &url);
	void evictOldest();
	void releaseTexture(const ImageInfo &info);
	void freePendingLocked();
	bool retryAsStill(const std::string &url, Network::RequestPriority priority);
	std::shared_ptr<Animation> decodeGif(std::string body, Utils::Image::TiledData &tiled);
	void uploadAnimationFrames();
	void decoderWorker();
	void animationWorker();
};

} // namespace UI

#endif // IMAGE_MANAGER_H
