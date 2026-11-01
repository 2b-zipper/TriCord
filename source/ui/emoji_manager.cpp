#include "ui/emoji_manager.h"
#include "log.h"
#include "network/network_manager.h"
#include "utils/image_utils.h"
#include "utils/redraw.h"
#include "utils/system_utils.h"
#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <malloc.h>
#include <unordered_set>

namespace UI {

namespace {

std::string emojiUrl(const std::string &emojiId, const char *extension) {
	return "https://media.discordapp.net/emojis/" + emojiId + extension + "?size=32";
}

} // namespace

EmojiManager &EmojiManager::getInstance() {
	static EmojiManager instance;
	return instance;
}

void EmojiManager::init() { loadEmojiData(); }

void EmojiManager::loadEmojiData() {
	if (dataLoaded) {
		return;
	}

	std::lock_guard<std::shared_mutex> lock(cacheMutex);

	std::ifstream f("romfs:/emoji_data.bin", std::ios::binary);
	if (!f) {
		Logger::log("Error: Failed to open romfs:/emoji_data.bin");
		dataLoaded = true;
		return;
	}

	char magic[4];
	f.read(magic, 4);
	if (std::memcmp(magic, "EMOJ", 4) != 0) {
		Logger::log("Error: Invalid magic in emoji_data.bin");
		return;
	}

	uint32_t version, count;
	f.read((char *)&version, 4);
	f.read((char *)&count, 4);

	std::vector<EmojiDataEntry> dataEntries(count);
	f.read((char *)dataEntries.data(), count * sizeof(EmojiDataEntry));
	f.close();

	std::vector<EmojiDataEntry> sortedEntries = dataEntries;
	std::sort(sortedEntries.begin(), sortedEntries.end(), [](const EmojiDataEntry &a, const EmojiDataEntry &b) {
		if (a.category != b.category) {
			return a.category < b.category;
		}
		return a.order < b.order;
	});

	std::unordered_set<std::string> availableFiles;
	DIR *dir = opendir("romfs:/twemoji17/");
	if (dir) {
		struct dirent *ent;
		while ((ent = readdir(dir)) != NULL) {
			std::string fname = ent->d_name;
			if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".png") {
				availableFiles.insert(fname.substr(0, fname.size() - 4));
			}
		}
		closedir(dir);
	}

	categories.clear();
	for (int i = 0; i < 8; i++) {
		categories.push_back({});
	}

	allCodepoints.clear();
	allCodepoints.reserve(count);

	for (const auto &entry : sortedEntries) {
		std::string hexStr = entry.hex;
		std::transform(hexStr.begin(), hexStr.end(), hexStr.begin(), ::tolower);

		std::string checkStr = hexStr;
		bool exists = (availableFiles.count(checkStr) > 0);
		if (!exists) {
			size_t fe0f = checkStr.find("-fe0f");
			if (fe0f != std::string::npos) {
				checkStr.erase(fe0f, 5);
				exists = (availableFiles.count(checkStr) > 0);
			}
		}

		size_t idx = allCodepoints.size();
		allCodepoints.push_back(hexStr);

		if (exists && entry.category >= 0 && entry.category < (int)categories.size()) {
			categories[entry.category].emojiIndices.push_back(idx);
		}
	}

	dataLoaded = true;
	Logger::log("[EmojiManager] Initialized with %d entries and %d categories", (int)count, (int)categories.size());
}

const std::vector<EmojiCategory> &EmojiManager::getCategories() {
	if (!dataLoaded) {
		loadEmojiData();
	}
	return categories;
}

const std::vector<std::string> &EmojiManager::getCodepoints() {
	if (!dataLoaded) {
		loadEmojiData();
	}
	return allCodepoints;
}

void EmojiManager::onCategoryChanged(const std::unordered_set<std::string> &keep) {
	std::unique_lock<std::shared_mutex> lock(cacheMutex);
	while (!priorityQueue.empty()) {
		std::string hex = priorityQueue.back();
		priorityQueue.pop_back();
		backgroundQueue.push_front(hex);
	}

	for (auto it = twemojiCache.begin(); it != twemojiCache.end();) {
		if (keep.find(it->first) == keep.end()) {
			if (it->second.tex) {
				C3D_TexDelete(it->second.tex);
				free(it->second.tex);
			}
			it = twemojiCache.erase(it);
		} else {
			++it;
		}
	}

	freePendingLocked(pendingTwemoji);
	pendingCv.notify_all();

	if (!backgroundQueue.empty()) {
		ensureLoaderLocked();
		loaderCv.notify_one();
	}
}

void EmojiManager::freePendingLocked(std::deque<PendingEmoji> &queue) {
	for (auto &pending : queue) {
		if (pending.tiled.pixels) {
			free(pending.tiled.pixels);
			pending.tiled.pixels = nullptr;
		}
	}
	queue.clear();
}

void EmojiManager::shutdown() {
	{
		std::unique_lock<std::shared_mutex> lock(cacheMutex);
		stopLoader = true;
	}
	loaderCv.notify_all();
	pendingCv.notify_all();
	loaderThread.join();

	std::unique_lock<std::shared_mutex> lock(cacheMutex);
	loaderStarted = false;
	stopLoader = false;
	freePendingLocked(pendingTwemoji);
	freePendingLocked(pendingCustom);
	for (auto &pair : emojiCache) {
		if (pair.second.tex) {
			C3D_TexDelete(pair.second.tex);
			free(pair.second.tex);
		}
	}
	for (auto &pair : twemojiCache) {
		if (pair.second.tex) {
			C3D_TexDelete(pair.second.tex);
			free(pair.second.tex);
		}
	}
	emojiCache.clear();
	twemojiCache.clear();
	priorityQueue.clear();
	backgroundQueue.clear();
	inQueue.clear();
	dataLoaded = false;
}

EmojiManager::~EmojiManager() { shutdown(); }

void EmojiManager::loaderWorker() {
	std::unique_lock<std::shared_mutex> lock(cacheMutex);
	while (!stopLoader) {
		std::string hex;
		if (!priorityQueue.empty()) {
			hex = priorityQueue.front();
			priorityQueue.pop_front();
		} else if (!backgroundQueue.empty()) {
			hex = backgroundQueue.front();
			backgroundQueue.pop_front();
		} else {
			loaderCv.wait(lock);
			continue;
		}

		inQueue.erase(hex);

		auto it = twemojiCache.find(hex);
		if (it == twemojiCache.end() || !it->second.isLoading) {
			continue;
		}
		lock.unlock();

		std::string path = "romfs:/twemoji17/" + hex + ".png";
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) {
			std::string s = hex;
			size_t p = 0;
			while ((p = s.find("-fe0f")) != std::string::npos) {
				s.erase(p, 5);
			}
			if (s != hex) {
				path = "romfs:/twemoji17/" + s + ".png";
				f = fopen(path.c_str(), "rb");
			}
		}

		Utils::Image::TiledData tiled;
		if (f) {
			fseek(f, 0, SEEK_END);
			size_t size = ftell(f);
			fseek(f, 0, SEEK_SET);
			std::vector<unsigned char> buffer(size);
			fread(buffer.data(), 1, size, f);
			fclose(f);
			tiled = Utils::Image::decodeToTiled(buffer.data(), size, TWEMOJI_DECODE_DIM, TWEMOJI_DECODE_DIM);
		}

		lock.lock();
		if (stopLoader) {
			if (tiled.pixels) {
				free(tiled.pixels);
			}
			break;
		}

		PendingEmoji pending;
		pending.key = hex;
		pending.tiled = tiled;
		pendingTwemoji.push_back(pending);

		pendingCv.wait(lock, [this] { return stopLoader || pendingTwemoji.size() < MAX_PENDING_EMOJI; });
	}
}

void EmojiManager::ensureLoaderLocked() {
	if (!loaderStarted) {
		loaderStarted = true;
		stopLoader = false;
		loaderThread.start([this] { loaderWorker(); }, 3, 32 * 1024, true);
	}
}

void EmojiManager::update() {
	std::unique_lock<std::shared_mutex> lock(cacheMutex);
	frameCounter++;
	ensureLoaderLocked();

	size_t twemojiUploads = 0;
	while (!pendingTwemoji.empty() && twemojiUploads < MAX_UPLOADS_PER_FRAME) {
		uploadPendingLocked(twemojiCache, pendingTwemoji.front());
		pendingTwemoji.pop_front();
		twemojiUploads++;
	}
	if (twemojiUploads > 0) {
		pendingCv.notify_one();
	}

	size_t customUploads = 0;
	while (!pendingCustom.empty() && customUploads < MAX_UPLOADS_PER_FRAME) {
		uploadPendingLocked(emojiCache, pendingCustom.front());
		pendingCustom.pop_front();
		customUploads++;
	}
	if (twemojiUploads > 0 || customUploads > 0) {
		Utils::requestRedraw();
	}
	evictLocked(twemojiCache, MAX_TWEMOJI_CACHE);
	evictLocked(emojiCache, MAX_CUSTOM_CACHE);
}

void EmojiManager::uploadPendingLocked(std::unordered_map<std::string, EmojiInfo> &cache, PendingEmoji &pending) {
	C3D_Tex *tex = nullptr;
	if (pending.tiled.pixels) {
		tex = Utils::Image::createTexture(pending.tiled);
		free(pending.tiled.pixels);
		pending.tiled.pixels = nullptr;
	}

	auto it = cache.find(pending.key);
	if (it != cache.end()) {
		it->second.tex = tex;
		it->second.originalW = pending.tiled.w;
		it->second.originalH = pending.tiled.h;
		it->second.isLoading = false;
		if (tex) {
			it->second.lastUsedFrame = frameCounter;
			it->second.attempts = 0;
		} else {
			it->second.attempts++;
			it->second.retryAt = osGetTime() + (uint64_t)3000 * it->second.attempts;
		}
	} else if (tex) {
		C3D_TexDelete(tex);
		free(tex);
	}
}

void EmojiManager::evictLocked(std::unordered_map<std::string, EmojiInfo> &cache, size_t limit) {
	if (cache.size() <= limit) {
		return;
	}
	while (cache.size() > limit - EVICT_BATCH) {
		auto oldestIt = cache.end();
		for (auto it = cache.begin(); it != cache.end(); ++it) {
			// Dropping an in-flight entry would only make the next frame fetch it again.
			if (it->second.isLoading) {
				continue;
			}
			if (oldestIt == cache.end() || it->second.lastUsedFrame < oldestIt->second.lastUsedFrame) {
				oldestIt = it;
			}
		}
		if (oldestIt == cache.end()) {
			break;
		}
		if (oldestIt->second.tex) {
			C3D_TexDelete(oldestIt->second.tex);
			free(oldestIt->second.tex);
		}
		cache.erase(oldestIt);
	}
}

size_t EmojiManager::getTwemojiCount() {
	std::shared_lock<std::shared_mutex> lock(cacheMutex);
	return twemojiCache.size();
}

size_t EmojiManager::getCustomCount() {
	std::shared_lock<std::shared_mutex> lock(cacheMutex);
	return emojiCache.size();
}

EmojiManager::EmojiInfo EmojiManager::getEmojiInfo(const std::string &emojiId, bool animated) {
	if (animated) {
		const ImageManager::ImageInfo gif = ImageManager::getInstance().getImageInfo(emojiUrl(emojiId, ".gif"));
		if (!gif.failed) {
			EmojiInfo info;
			info.tex = gif.tex;
			info.originalW = gif.originalW;
			info.originalH = gif.originalH;
			info.animation = gif.animation;
			return info;
		}
	}

	std::shared_lock<std::shared_mutex> lock(cacheMutex);
	auto it = emojiCache.find(emojiId);
	if (it != emojiCache.end()) {
		if (it->second.tex) {
			it->second.lastUsedFrame = frameCounter;
		}
		return it->second;
	}
	return EmojiInfo();
}

void EmojiManager::prefetchEmoji(const std::string &emojiId, bool animated) {
	if (emojiId.empty()) {
		return;
	}

	if (animated) {
		ImageManager &images = ImageManager::getInstance();
		const std::string url = emojiUrl(emojiId, ".gif");
		if (!images.getImageInfo(url).failed) {
			images.prefetch(url, 0, 0, Network::RequestPriority::INTERACTIVE);
			return;
		}
	}

	{
		std::unique_lock<std::shared_mutex> lock(cacheMutex);
		auto it = emojiCache.find(emojiId);
		if (it == emojiCache.end()) {
			EmojiInfo placeholder;
			placeholder.isLoading = true;
			placeholder.lastUsedFrame = frameCounter;
			emojiCache[emojiId] = placeholder;
		} else if (!it->second.tex && !it->second.isLoading && it->second.attempts < MAX_CUSTOM_ATTEMPTS &&
		           osGetTime() >= it->second.retryAt) {
			it->second.isLoading = true;
			it->second.lastUsedFrame = frameCounter;
		} else {
			return;
		}
	}

	Network::NetworkManager::getInstance().enqueue(
	    emojiUrl(emojiId, ".png"), "GET", "", Network::RequestPriority::INTERACTIVE,
	    [this, emojiId](Network::HttpResponse &resp) {
		    Utils::Image::TiledData tiled;
		    if (resp.statusCode == 200 && !resp.body.empty()) {
			    tiled = Utils::Image::decodeToTiled((const unsigned char *)resp.body.data(), resp.body.size(),
			                                        TWEMOJI_DECODE_DIM, TWEMOJI_DECODE_DIM);
		    } else {
			    Logger::log("[Emoji] Fetch failed for %s (status %d)", emojiId.c_str(), resp.statusCode);
		    }
		    std::unique_lock<std::shared_mutex> lock(cacheMutex);
		    pendingCustom.push_back({emojiId, tiled});
	    });
}

void EmojiManager::prefetchEmojisFromText(const std::string &text) {
	for (size_t cursor = 0; cursor < text.length(); ++cursor) {
		if (text[cursor] != '<') {
			continue;
		}
		if (cursor + 6 >= text.length()) {
			continue;
		}
		bool isAnimated = (text[cursor + 1] == 'a');
		if (text[cursor + 1] != ':' && !isAnimated) {
			continue;
		}
		size_t startPos = cursor + (isAnimated ? 3 : 2);
		size_t secondColon = text.find(':', startPos);
		if (secondColon == std::string::npos) {
			continue;
		}
		size_t closeBracket = text.find('>', secondColon);
		if (closeBracket == std::string::npos) {
			continue;
		}
		std::string emojiId = text.substr(secondColon + 1, closeBracket - secondColon - 1);
		if (!emojiId.empty()) {
			prefetchEmoji(emojiId, isAnimated);
		}
		cursor = closeBracket;
	}
}

EmojiManager::EmojiInfo EmojiManager::getTwemojiInfo(const std::string &codepointHex) {
	{
		std::shared_lock<std::shared_mutex> lock(cacheMutex);
		auto it = twemojiCache.find(codepointHex);
		if (it != twemojiCache.end()) {
			it->second.lastUsedFrame = frameCounter;
			if (!it->second.isLoading || inQueue.find(codepointHex) != inQueue.end()) {
				return it->second;
			}
		}
	}

	std::unique_lock<std::shared_mutex> lock(cacheMutex);
	auto it = twemojiCache.find(codepointHex);
	if (it != twemojiCache.end()) {
		it->second.lastUsedFrame = frameCounter;
		if (!it->second.isLoading) {
			return it->second;
		}
		if (inQueue.find(codepointHex) == inQueue.end()) {
			priorityQueue.push_back(codepointHex);
			inQueue.insert(codepointHex);
			ensureLoaderLocked();
			loaderCv.notify_one();
		}
		return it->second;
	}

	EmojiInfo pending;
	pending.isLoading = true;
	pending.lastUsedFrame = frameCounter;
	twemojiCache[codepointHex] = pending;

	priorityQueue.push_back(codepointHex);
	inQueue.insert(codepointHex);
	ensureLoaderLocked();
	loaderCv.notify_one();

	return pending;
}

} // namespace UI
