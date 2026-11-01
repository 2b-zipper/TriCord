#include "ui/image_manager.h"
#include "log.h"
#include "network/network_manager.h"
#include "utils/gif_decoder.h"
#include "utils/image_utils.h"
#include "utils/redraw.h"
#include "utils/system_utils.h"

#include <algorithm>
#include <cstring>

namespace UI {

namespace {

constexpr size_t ANIMATION_STACK = 8 * 1024;
constexpr s64 ANIMATION_POLL_NS = 16 * 1000000LL;

// GIFs often run faster than the screen is worth redrawing for. Frames are
// still decoded on time, so playback speed does not change.
int minFrameMs() {
	static const int ms = Utils::System::isNew3DS() ? 33 : 66;
	return ms;
}

bool isDiscordMedia(const std::string &url) {
	return url.find("cdn.discordapp.com") != std::string::npos ||
	       url.find("media.discordapp.net") != std::string::npos || url.find("images-ext-") != std::string::npos;
}

bool isEmoji(const std::string &url) { return url.find("/emojis/") != std::string::npos; }

} // namespace

struct ImageManager::Animation {
	Animation(std::string body, int maxDim) : gif(std::move(body), maxDim) {}

	Utils::Gif::Decoder gif;
	C3D_Tex *tex = nullptr;
	u64 dueAt = 0;
	std::atomic<bool> frameReady{false};
	std::atomic<bool> drawn{false};
};

ImageManager &ImageManager::getInstance() {
	static ImageManager instance;
	return instance;
}

size_t ImageManager::getCacheBudget() {
	static const size_t budget =
	    Utils::System::linearHeapSize() >= 32 * 1024 * 1024 ? 12 * 1024 * 1024 : 8 * 1024 * 1024;
	return budget;
}

ImageManager::~ImageManager() {
	shutdown();
	clear();
}

void ImageManager::init() {
	if (!decoderThread.joinable()) {
		stopDecoder = false;
		decoderThread.start([this] { decoderWorker(); }, 3, 32 * 1024, true);
	}
	if (!animationThread.joinable()) {
		stopAnimation = false;
		animationThread.start([this] { animationWorker(); }, 3, ANIMATION_STACK, true);
	}
}

void ImageManager::shutdown() {
	{
		std::lock_guard<std::mutex> lock(decodeMutex);
		stopDecoder = true;
	}
	decodeCv.notify_all();
	pendingCv.notify_all();
	if (decoderThread.joinable()) {
		decoderThread.join();
	}
	{
		std::lock_guard<std::mutex> lock(decodeMutex);
		jobQueue.clear();
	}
	{
		std::lock_guard<std::mutex> lock(animationMutex);
		stopAnimation = true;
	}
	animationCv.notify_all();
	if (animationThread.joinable()) {
		animationThread.join();
	}

	clear();
}

void ImageManager::clear() {
	std::lock_guard<std::mutex> lock(cacheMutex);
	for (auto &pair : textureCache) {
		releaseTexture(pair.second);
	}
	textureCache.clear();
	lruList.clear();
	fetchingUrls.clear();
	stillFallback.clear();
	freePendingLocked();
	currentCacheBytes = 0;
	{
		std::lock_guard<std::mutex> lock(decodeMutex);
		decodeQueue.clear();
	}
	currentSessionId++;
}

void ImageManager::releaseTexture(const ImageInfo &info) {
	if (info.animation) {
		std::lock_guard<std::mutex> lock(animationMutex);
		animations.erase(std::remove(animations.begin(), animations.end(), info.animation), animations.end());
	}
	if (info.tex) {
		C3D_TexDelete(info.tex);
		free(info.tex);
	}
}

void ImageManager::freePendingLocked() {
	for (auto &pending : pendingTextures) {
		if (pending.tiled.pixels) {
			free(pending.tiled.pixels);
			pending.tiled.pixels = nullptr;
		}
	}
	pendingTextures.clear();
	pendingCv.notify_all();
}

void ImageManager::touchImage(const std::string &url) {
	if (url.find("http") != 0) {
		return; // Don't LRU local images
	}

	lruList.remove(url);
	lruList.push_front(url);
}

void ImageManager::evictOldest() {
	if (lruList.empty()) {
		return;
	}

	std::string url = lruList.back();
	lruList.pop_back();

	auto it = textureCache.find(url);
	if (it != textureCache.end()) {
		currentCacheBytes -= it->second.vramSize;
		releaseTexture(it->second);
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
			currentCacheBytes -= it->second.vramSize;
			releaseTexture(it->second);
			it = textureCache.erase(it);
		} else {
			++it;
		}
	}
	lruList.clear();
	fetchingUrls.clear();
	freePendingLocked();
	{
		std::lock_guard<std::mutex> lock2(decodeMutex);
		decodeQueue.clear();
	}
	currentSessionId++;
}

size_t ImageManager::getCacheBytes() {
	std::lock_guard<std::mutex> lock(cacheMutex);
	return currentCacheBytes;
}

size_t ImageManager::getCacheCount() {
	std::lock_guard<std::mutex> lock(cacheMutex);
	return textureCache.size();
}

C3D_Tex *ImageManager::getImage(const std::string &url) {
	if (url.empty()) {
		return nullptr;
	}

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

ImageManager::ImageInfo ImageManager::getImageInfo(std::string_view url) {
	std::lock_guard<std::mutex> lock(cacheMutex);
	auto it = textureCache.find(url);
	if (it != textureCache.end()) {
		touchImage(it->first);
		return it->second;
	}
	return ImageInfo();
}

C3D_Tex *ImageManager::getLocalImage(std::string_view path, bool noResize) {
	if (path.empty()) {
		return nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(cacheMutex);
		auto it = textureCache.find(path);
		if (it != textureCache.end()) {
			return it->second.tex;
		}
	}

	std::string pathStr(path);
	Logger::log("[Image] Loading local: %s", pathStr.c_str());

	FILE *f = fopen(pathStr.c_str(), "rb");
	if (!f) {
		Logger::log("[Image] Failed to open local file: %s", pathStr.c_str());
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

	C3D_Tex *tex = Utils::Image::loadTextureFromMemory((const unsigned char *)data.data(), size, outW, outH, noResize);
	if (tex) {
		ImageInfo info;
		info.tex = tex;
		info.originalW = outW;
		info.originalH = outH;
		std::lock_guard<std::mutex> lock(cacheMutex);
		textureCache[pathStr] = info;
		return tex;
	}

	return nullptr;
}

void ImageManager::prefetch(const std::string &url, int origW, int origH, Network::RequestPriority priority) {
	if (url.empty()) {
		return;
	}
	bool animated;
	{
		std::lock_guard<std::mutex> lock(cacheMutex);
		auto it = textureCache.find(url);
		if (it != textureCache.end()) {
			if (!it->second.failed) {
				return;
			}
			textureCache.erase(it);
		}
		if (fetchingUrls.find(url) != fetchingUrls.end()) {
			return;
		}

		fetchingUrls.insert(url);
		animated =
		    !stillFallback.count(url) && (Utils::Gif::hasGifPath(url) || url.find("format=gif") != std::string::npos);
	}

	std::string optimizedUrl = url;

	if (optimizedUrl.find("cdn.discordapp.com") != std::string::npos) {
		size_t pos = optimizedUrl.find("cdn.discordapp.com");
		optimizedUrl.replace(pos, 18, "media.discordapp.net");
	}

	if (optimizedUrl.find("media.discordapp.net") != std::string::npos ||
	    optimizedUrl.find("images-ext-") != std::string::npos) {
		int targetW = animated ? Utils::Image::maxAnimatedDim() : Utils::Image::maxRemoteDim();
		int targetH = targetW;
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

		if (optimizedUrl.find("avatars/") != std::string::npos || optimizedUrl.find("icons/") != std::string::npos ||
		    optimizedUrl.find("banners/") != std::string::npos || optimizedUrl.find("splashes/") != std::string::npos ||
		    optimizedUrl.find("app-icons/") != std::string::npos) {
			int p2size = 64;
			if (std::max(targetW, targetH) > 64) {
				p2size = 128;
			}
			if (std::max(targetW, targetH) > 128) {
				p2size = 256;
			}

			if (optimizedUrl.find("size=") == std::string::npos) {
				optimizedUrl += separator + "size=" + std::to_string(p2size);
				separator = "&";
			}
			if (optimizedUrl.find("format=") == std::string::npos) {
				optimizedUrl += separator + "format=jpeg";
			}
		} else {
			if (optimizedUrl.find("width=") == std::string::npos) {
				optimizedUrl += separator + "width=" + std::to_string(targetW) + "&height=" + std::to_string(targetH);
				separator = "&";
			}
			if (!animated && optimizedUrl.find("format=") == std::string::npos) {
				optimizedUrl += separator + "format=jpeg";
			}
		}
	}

	int sessionId = currentSessionId;
	Network::NetworkManager::getInstance().enqueue(
	    optimizedUrl, "GET", "", priority, [this, url, sessionId, priority](Network::HttpResponse &resp) {
		    if (currentSessionId != sessionId) {
			    return;
		    }

		    if (resp.success && resp.statusCode == 200 && !resp.body.empty()) {
			    DecodeRequest req;
			    req.url = url;
			    req.body = std::move(resp.body);
			    req.sessionId = sessionId;
			    req.priority = priority;

			    // TLS handshakes can hold the decoder's core for seconds, and an emoji frame is cheap.
			    if (isEmoji(url)) {
				    decode(req);
				    return;
			    }

			    std::lock_guard<std::mutex> lock(decodeMutex);
			    decodeQueue.push_back(std::move(req));
			    decodeCv.notify_one();
		    } else {
			    Logger::log("[Image] Fetch failed for %s. Status: %d, Body size: %zu", url.c_str(), resp.statusCode,
			                resp.body.size());
			    if (retryAsStill(url, priority)) {
				    return;
			    }
			    std::lock_guard<std::mutex> lock(cacheMutex);
			    ImageInfo info;
			    info.failed = true;
			    textureCache[url] = info;
			    fetchingUrls.erase(url);
		    }
	    });
}

bool ImageManager::retryAsStill(const std::string &url, Network::RequestPriority priority) {
	// A JPEG would lose an emoji's transparency, and EmojiManager falls back to its PNG instead.
	if (!isDiscordMedia(url) || !Utils::Gif::hasGifPath(url) || isEmoji(url)) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(cacheMutex);
		if (!stillFallback.insert(url).second) {
			return false;
		}
		fetchingUrls.erase(url);
	}
	prefetch(url, 0, 0, priority);
	return true;
}

std::shared_ptr<ImageManager::Animation> ImageManager::decodeGif(std::string body, Utils::Image::TiledData &tiled) {
	const bool tooLarge = body.size() > Utils::Image::maxAnimatedBytes();
	auto animation = std::make_shared<Animation>(std::move(body), Utils::Image::maxAnimatedDim());
	if (!animation->gif.valid()) {
		return nullptr;
	}

	const int delayMs = animation->gif.next();
	tiled = animation->gif.copyPixels();
	if (tooLarge || animation->gif.frameCount() < 2) {
		return nullptr;
	}
	animation->dueAt = osGetTime() + delayMs;
	return animation;
}

void ImageManager::runOnDecoder(std::function<void()> job) {
	{
		std::lock_guard<std::mutex> lock(decodeMutex);
		jobQueue.push_back(std::move(job));
	}
	decodeCv.notify_one();
}

void ImageManager::decode(DecodeRequest &req) {
	Utils::Image::TiledData tiled;
	std::shared_ptr<Animation> animation;
	if (Utils::Gif::isGif(req.body)) {
		animation = decodeGif(std::move(req.body), tiled);
	} else {
		tiled = Utils::Image::decodeToTiled((const unsigned char *)req.body.data(), req.body.size());
	}

	if (!tiled.pixels && retryAsStill(req.url, req.priority)) {
		return;
	}

	PendingTexture pending;
	pending.url = req.url;
	pending.tiled = tiled;
	pending.animation = std::move(animation);
	pending.success = (tiled.pixels != nullptr);
	pending.width = tiled.w;
	pending.height = tiled.h;

	std::lock_guard<std::mutex> lock(cacheMutex);
	if (currentSessionId != req.sessionId) {
		if (tiled.pixels) {
			free(tiled.pixels);
		}
		return;
	}
	pendingTextures.push_back(pending);
}

void ImageManager::decoderWorker() {
	while (true) {
		DecodeRequest req;
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(decodeMutex);
			decodeCv.wait(lock, [this] { return stopDecoder || !jobQueue.empty() || !decodeQueue.empty(); });
			if (stopDecoder) {
				return;
			}
			if (!jobQueue.empty()) {
				job = std::move(jobQueue.front());
				jobQueue.pop_front();
			} else {
				req = std::move(decodeQueue.front());
				decodeQueue.pop_front();
			}
		}

		if (job) {
			job();
			continue;
		}

		if (currentSessionId != req.sessionId) {
			continue;
		}

		{
			std::unique_lock<std::mutex> lock(cacheMutex);
			pendingCv.wait(lock, [this] { return stopDecoder || pendingTextures.size() < MAX_PENDING_TEXTURES; });
		}
		if (stopDecoder || currentSessionId != req.sessionId) {
			continue;
		}

		decode(req);
	}
}

void ImageManager::animationWorker() {
	const auto waitingForFrame = [](const std::shared_ptr<Animation> &animation) {
		return animation->drawn && !animation->frameReady;
	};

	while (true) {
		std::vector<std::shared_ptr<Animation>> playing;
		{
			std::unique_lock<std::mutex> lock(animationMutex);
			animationCv.wait(lock, [&] {
				return stopAnimation || std::any_of(animations.begin(), animations.end(), waitingForFrame);
			});
			if (stopAnimation) {
				return;
			}
			playing = animations;
		}

		const u64 now = osGetTime();
		for (const auto &animation : playing) {
			if (!waitingForFrame(animation) || now < animation->dueAt) {
				continue;
			}

			int shownMs = 0;
			do {
				shownMs += animation->gif.next();
			} while (shownMs < minFrameMs());

			animation->dueAt = std::max(animation->dueAt + shownMs, now);
			animation->frameReady = true;
		}
		svcSleepThread(ANIMATION_POLL_NS);
	}
}

void ImageManager::uploadAnimationFrames() {
	std::lock_guard<std::mutex> lock(animationMutex);
	for (const auto &animation : animations) {
		if (!animation->frameReady) {
			continue;
		}
		memcpy(animation->tex->data, animation->gif.pixels(), animation->gif.pixelBytes());
		GSPGPU_FlushDataCache(animation->tex->data, animation->gif.pixelBytes());

		animation->drawn = false;
		animation->frameReady = false;
		Utils::requestRedraw();
	}
}

void ImageManager::markDrawn(const std::shared_ptr<Animation> &animation) {
	if (!animation || animation->drawn) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(animationMutex);
		animation->drawn = true;
	}
	animationCv.notify_one();
}

void ImageManager::update() {
	uploadAnimationFrames();

	PendingTexture p;
	{
		std::lock_guard<std::mutex> lock(cacheMutex);
		if (pendingTextures.empty()) {
			return;
		}
		p = std::move(pendingTextures.front());
		pendingTextures.pop_front();
	}
	pendingCv.notify_one();

	bool decoded = p.success && p.tiled.pixels;
	const size_t imageBytes = p.tiled.vramSize + (p.animation ? p.animation->gif.memoryBytes() : 0);

	if (decoded) {
		std::lock_guard<std::mutex> lock(cacheMutex);
		while (currentCacheBytes + imageBytes > getCacheBudget() && !lruList.empty()) {
			evictOldest();
		}
	}

	ImageInfo info;
	if (decoded) {
		C3D_Tex *tex = nullptr;
		while (!(tex = Utils::Image::createTexture(p.tiled))) {
			std::lock_guard<std::mutex> lock(cacheMutex);
			if (lruList.empty()) {
				break;
			}
			Logger::log("[Image] C3D_TexInit failed. Evicting texture to free VRAM...");
			evictOldest();
		}

		if (tex) {
			info.tex = tex;
			info.originalW = p.width;
			info.originalH = p.height;
			info.vramSize = imageBytes;
			if (p.animation) {
				p.animation->tex = tex;
				info.animation = p.animation;
				std::lock_guard<std::mutex> lock(animationMutex);
				animations.push_back(p.animation);
			}
		} else {
			info.failed = true;
			Logger::log("[Image] C3D_TexInit completely failed for %s", p.url.c_str());
		}
	} else {
		info.failed = true;
	}

	if (p.tiled.pixels) {
		free(p.tiled.pixels);
		p.tiled.pixels = nullptr;
	}

	std::lock_guard<std::mutex> lock(cacheMutex);
	fetchingUrls.erase(p.url);
	textureCache[p.url] = info;
	if (info.tex) {
		currentCacheBytes += info.vramSize;
		touchImage(p.url);
	}
	generation++;
	Utils::requestRedraw();
}

} // namespace UI
