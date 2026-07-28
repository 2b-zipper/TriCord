#include "ui/emoji_manager.h"
#include "log.h"
#include "network/network_manager.h"
#include "utils/image_utils.h"
#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <malloc.h>
#include <unordered_set>

namespace UI {

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

	freePendingLocked();
	pendingCv.notify_all();

	if (!backgroundQueue.empty()) {
		ensureLoaderLocked();
		loaderCv.notify_one();
	}
}

void EmojiManager::freePendingLocked() {
	for (auto &pending : pendingEmoji) {
		if (pending.tiled.pixels) {
			free(pending.tiled.pixels);
			pending.tiled.pixels = nullptr;
		}
	}
	pendingEmoji.clear();
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
	freePendingLocked();
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
		pending.hex = hex;
		pending.tiled = tiled;
		pendingEmoji.push_back(pending);

		pendingCv.wait(lock, [this] { return stopLoader || pendingEmoji.size() < MAX_PENDING_EMOJI; });
	}
}

void EmojiManager::ensureLoaderLocked() {
	if (!loaderStarted) {
		loaderStarted = true;
		stopLoader = false;
		loaderThread.start([this] { loaderWorker(); }, 3);
	}
}

void EmojiManager::update() {
	std::unique_lock<std::shared_mutex> lock(cacheMutex);
	frameCounter++;
	ensureLoaderLocked();

	size_t uploads = 0;
	while (!pendingEmoji.empty() && uploads < MAX_UPLOADS_PER_FRAME) {
		PendingEmoji p = pendingEmoji.front();
		pendingEmoji.pop_front();
		uploads++;

		C3D_Tex *tex = nullptr;
		if (p.tiled.pixels) {
			tex = (C3D_Tex *)malloc(sizeof(C3D_Tex));
			if (tex && C3D_TexInit(tex, p.tiled.p2w, p.tiled.p2h, GPU_RGBA8)) {
				C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
				memcpy(tex->data, p.tiled.pixels, p.tiled.vramSize);
				GSPGPU_FlushDataCache(tex->data, p.tiled.vramSize);
			} else {
				free(tex);
				tex = nullptr;
			}
			free(p.tiled.pixels);
			p.tiled.pixels = nullptr;
		}

		auto it = twemojiCache.find(p.hex);
		if (it != twemojiCache.end()) {
			it->second.tex = tex;
			it->second.originalW = p.tiled.w;
			it->second.originalH = p.tiled.h;
			it->second.isLoading = false;
			it->second.lastUsedFrame = frameCounter;
		} else if (tex) {
			C3D_TexDelete(tex);
			free(tex);
		}
	}
	if (uploads > 0) {
		pendingCv.notify_one();
	}

	if (twemojiCache.size() > MAX_TWEMOJI_CACHE) {
		while (twemojiCache.size() > MAX_TWEMOJI_CACHE - 50) {
			uint32_t oldestFrame = 0xFFFFFFFF;
			auto oldestIt = twemojiCache.end();

			for (auto it = twemojiCache.begin(); it != twemojiCache.end(); ++it) {
				if (it->second.lastUsedFrame < oldestFrame) {
					oldestFrame = it->second.lastUsedFrame;
					oldestIt = it;
				}
			}

			if (oldestIt != twemojiCache.end()) {
				if (oldestIt->second.tex) {
					C3D_TexDelete(oldestIt->second.tex);
					free(oldestIt->second.tex);
				}
				twemojiCache.erase(oldestIt);
			} else {
				break;
			}
		}
	}
}

EmojiManager::EmojiInfo EmojiManager::getEmojiInfo(const std::string &emojiId) {
	std::shared_lock<std::shared_mutex> lock(cacheMutex);
	auto it = emojiCache.find(emojiId);
	if (it != emojiCache.end()) {
		it->second.lastUsedFrame = frameCounter;
		return it->second;
	}
	return EmojiInfo();
}

void EmojiManager::prefetchEmoji(const std::string &emojiId) {
	if (emojiId.empty()) {
		return;
	}
	{
		std::unique_lock<std::shared_mutex> lock(cacheMutex);
		if (emojiCache.count(emojiId)) {
			return;
		}
		EmojiInfo placeholder;
		placeholder.isLoading = true;
		emojiCache[emojiId] = placeholder;
	}

	std::string url = "https://media.discordapp.net/emojis/" + emojiId + ".png?size=32";
	Network::NetworkManager::getInstance().enqueue(
	    url, "GET", "", Network::RequestPriority::INTERACTIVE, [this, emojiId](const Network::HttpResponse &resp) {
		    if (resp.statusCode == 200 && !resp.body.empty()) {
			    int w, h;
			    C3D_Tex *tex = Utils::Image::loadTextureFromMemory((const unsigned char *)resp.body.data(),
			                                                       resp.body.size(), w, h);
			    if (tex) {
				    std::unique_lock<std::shared_mutex> lock(cacheMutex);
				    EmojiInfo info;
				    info.tex = tex;
				    info.originalW = w;
				    info.originalH = h;
				    info.lastUsedFrame = frameCounter;
				    info.isLoading = false;
				    emojiCache[emojiId] = info;
			    }
		    }
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
			prefetchEmoji(emojiId);
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
