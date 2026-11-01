#pragma once
#include "utils/image_utils.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Utils {
namespace Gif {

bool isGif(const std::string &data);
bool hasGifPath(const std::string &url);

// Frames are decoded one at a time onto a single canvas, so a long GIF costs
// its file size rather than its frame count. The canvas stays in texture layout
// as RGBA5551, so a finished frame uploads with a memcpy.
class Decoder {
  public:
	Decoder(std::string source, int maxDim);
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	bool valid() const { return !canvas.empty(); }
	int frameCount() const { return frames; }
	// Counts the backup canvas up front, since it is only allocated once a frame asks for it.
	size_t memoryBytes() const { return data.size() + pixelBytes() * 2; }

	// Composites the next frame, wrapping at the end, and returns how long it
	// stays up in milliseconds.
	int next();

	const void *pixels() const { return canvas.data(); }
	size_t pixelBytes() const { return canvas.size() * sizeof(uint16_t); }
	Image::TiledData copyPixels() const;

  private:
	static constexpr int DEFAULT_DELAY_MS = 100;

	struct Rect {
		int left = 0;
		int top = 0;
		int width = 0;
		int height = 0;
	};

	struct GraphicControl {
		int disposal = 0;
		int delayMs = DEFAULT_DELAY_MS;
		int transparent = -1;
	};

	bool scan();
	void buildSampling(int maxDim);
	void rewind();
	void dispose();
	void readExtension(GraphicControl &control);
	void decodeImage(const GraphicControl &control);
	void decodeRaster(const Rect &rect, const uint16_t *palette, int transparent, bool interlaced);
	void skipSubBlocks(size_t &position) const;
	uint16_t *rowAt(int y);

	std::string data;
	const uint8_t *in = nullptr;
	size_t size = 0;
	size_t firstBlock = 0;
	size_t cursor = 0;

	int screenW = 0;
	int screenH = 0;
	int targetW = 0;
	int targetH = 0;
	int p2w = 0;
	int p2h = 0;
	int frames = 0;

	int disposal = 0;
	Rect disposalRect;

	uint16_t globalPalette[256] = {};
	uint16_t localPalette[256] = {};

	std::vector<uint16_t> canvas;
	std::vector<uint16_t> backup;
	std::vector<int> columns;
	std::vector<int> rows;
};

} // namespace Gif
} // namespace Utils
