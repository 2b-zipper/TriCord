#include "utils/lottie_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <thorvg.h>

namespace Utils {
namespace Lottie {

namespace {

constexpr int MIN_TEXTURE_SIDE = 8;

constexpr int MORTON_X[8] = {0, 1, 4, 5, 16, 17, 20, 21};
constexpr int MORTON_Y[8] = {0, 2, 8, 10, 32, 34, 40, 42};

// ThorVG renders premultiplied, and citro2d blends with straight alpha.
uint32_t toTexel(uint32_t abgr) {
	static const auto reciprocal = [] {
		std::array<uint32_t, 256> table{};
		for (uint32_t a = 1; a < 256; a++) {
			table[a] = (255u * 65536u + a / 2) / a;
		}
		return table;
	}();

	const uint32_t a = abgr >> 24;
	if (a == 0) {
		return 0;
	}
	if (a == 255) {
		return __builtin_bswap32(abgr);
	}
	const uint32_t inv = reciprocal[a];
	const uint32_t r = std::min<uint32_t>(((abgr & 0xFF) * inv + 0x8000) >> 16, 255);
	const uint32_t g = std::min<uint32_t>((((abgr >> 8) & 0xFF) * inv + 0x8000) >> 16, 255);
	const uint32_t b = std::min<uint32_t>((((abgr >> 16) & 0xFF) * inv + 0x8000) >> 16, 255);
	return (r << 24) | (g << 16) | (b << 8) | a;
}

} // namespace

bool hasLottiePath(const std::string &url) {
	const size_t end = std::min(url.find('?'), url.size());
	return url.find("/stickers/") != std::string::npos && end >= 5 && url.compare(end - 5, 5, ".json") == 0;
}

Player::Player(std::string json, int maxDim) {
	if (tvg::Initializer::init(0) != tvg::Result::Success) {
		return;
	}
	initialized = true;

	const size_t heapBefore = mallinfo().uordblks;
	animation = tvg::Animation::gen();
	tvg::Picture *picture = animation->picture();
	// Borrowed: ThorVG parses the text in place and keeps none of it once loaded.
	if (picture->load(json.data(), (uint32_t)json.size(), "lottie+json", nullptr, false) != tvg::Result::Success) {
		return;
	}
	json = std::string();

	float w = 0.0f;
	float h = 0.0f;
	picture->size(&w, &h);
	totalFrames = animation->totalFrame();
	periodMs = (u64)(animation->duration() * 1000.0f);
	if (w <= 0.0f || h <= 0.0f || totalFrames < 1.0f || periodMs == 0) {
		return;
	}

	const float scale = (float)maxDim / std::max(w, h);
	width = std::max(1, (int)lroundf(w * scale));
	height = std::max(1, (int)lroundf(h * scale));
	picture->size((float)width, (float)height);

	p2w = MIN_TEXTURE_SIDE;
	while (p2w < width) {
		p2w *= 2;
	}
	p2h = MIN_TEXTURE_SIDE;
	while (p2h < height) {
		p2h *= 2;
	}
	linear.resize((size_t)width * height);
	tiled.assign((size_t)p2w * p2h, 0);

	tvg::SwCanvas *sw = tvg::SwCanvas::gen();
	if (!sw) {
		return;
	}
	if (sw->target(linear.data(), width, width, height, tvg::ColorSpace::ABGR8888) != tvg::Result::Success ||
	    sw->add(picture) != tvg::Result::Success) {
		delete sw;
		return;
	}
	canvas = sw;

	if (!render(0.0f)) {
		delete canvas;
		canvas = nullptr;
		return;
	}
	const size_t heapAfter = mallinfo().uordblks;
	modelBytes = heapAfter > heapBefore ? heapAfter - heapBefore : 0;
	startedAt = osGetTime();
}

Player::~Player() {
	delete canvas;
	delete animation;
	if (initialized) {
		tvg::Initializer::term();
	}
}

bool Player::render(float frame) {
	animation->frame(frame);
	if (canvas->update() != tvg::Result::Success || canvas->draw(true) != tvg::Result::Success ||
	    canvas->sync() != tvg::Result::Success) {
		return false;
	}

	const int tilesPerRow = p2w >> 3;
	for (int y = 0; y < height; y++) {
		const uint32_t *src = &linear[(size_t)y * width];
		uint32_t *row = &tiled[((size_t)((y >> 3) * tilesPerRow) << 6) + MORTON_Y[y & 7]];
		for (int x = 0; x < width; x++) {
			row[((x >> 3) << 6) + MORTON_X[x & 7]] = toTexel(src[x]);
		}
	}
	return true;
}

int Player::next(int frameMs) {
	const u64 elapsed = (osGetTime() - startedAt) % periodMs;
	render(std::min((float)elapsed * totalFrames / (float)periodMs, totalFrames - 0.001f));
	return frameMs;
}

Image::TiledData Player::copyPixels() const {
	Image::TiledData copy;
	copy.pixels = (u32 *)malloc(pixelBytes());
	if (!copy.pixels) {
		return copy;
	}
	memcpy(copy.pixels, pixels(), pixelBytes());
	copy.w = width;
	copy.h = height;
	copy.p2w = p2w;
	copy.p2h = p2h;
	copy.vramSize = pixelBytes();
	copy.format = GPU_RGBA8;
	return copy;
}

} // namespace Lottie
} // namespace Utils
