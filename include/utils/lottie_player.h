#pragma once
#include "utils/image_utils.h"
#include <3ds.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tvg {
struct Animation;
struct SwCanvas;
} // namespace tvg

namespace Utils {
namespace Lottie {

bool hasLottiePath(const std::string &url);

// Renders onto a canvas kept in texture layout as RGBA8, so a finished frame
// uploads with a memcpy. ThorVG runs without worker threads and keeps
// per-thread state, so every call, construction and destruction included, must
// come from the same thread.
class Player {
  public:
	Player(std::string json, int maxDim);
	~Player();
	Player(const Player &) = delete;
	Player &operator=(const Player &) = delete;

	bool valid() const { return canvas != nullptr; }
	size_t memoryBytes() const { return modelBytes + (linear.size() + tiled.size()) * sizeof(uint32_t); }

	// Renders the frame due now, so a slow render skips frames instead of
	// slowing playback, and returns how long it stays up.
	int next(int frameMs);

	const void *pixels() const { return tiled.data(); }
	size_t pixelBytes() const { return tiled.size() * sizeof(uint32_t); }
	Image::TiledData copyPixels() const;

  private:
	bool render(float frame);

	bool initialized = false;
	tvg::Animation *animation = nullptr;
	tvg::SwCanvas *canvas = nullptr;
	size_t modelBytes = 0;
	float totalFrames = 0.0f;
	u64 periodMs = 0;
	u64 startedAt = 0;

	int width = 0;
	int height = 0;
	int p2w = 0;
	int p2h = 0;
	std::vector<uint32_t> linear;
	std::vector<uint32_t> tiled;
};

} // namespace Lottie
} // namespace Utils
