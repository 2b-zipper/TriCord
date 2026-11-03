#pragma once
#include "utils/image_utils.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct z_stream_s;

namespace Utils {
namespace Apng {

// True only for animations this decoder can play; anything else is left to the still decoder.
bool isApng(const std::string &data);

// Frames are inflated one at a time onto a full-size canvas, so a long animation
// costs its file size rather than its frame count. The canvas is box-filtered
// into texture layout as RGBA8, so a finished frame uploads with a memcpy.
class Decoder {
  public:
	Decoder(std::string source, int maxDim);
	~Decoder();
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	bool valid() const { return !output.empty(); }
	int frameCount() const { return (int)frames.size(); }
	size_t memoryBytes() const;

	// Composites the next frame, wrapping at the end, and returns how long it
	// stays up in milliseconds.
	int next();

	const void *pixels() const { return output.data(); }
	size_t pixelBytes() const { return output.size() * sizeof(uint32_t); }
	Image::TiledData copyPixels() const;

  private:
	struct Rect {
		int left = 0;
		int top = 0;
		int width = 0;
		int height = 0;
	};

	struct Chunk {
		size_t offset;
		size_t length;
	};

	struct Frame {
		Rect rect;
		int delayMs = 0;
		uint8_t dispose = 0;
		uint8_t blend = 0;
		std::vector<Chunk> chunks;
	};

	bool scan();
	void buildSampling(int maxDim);
	void rewind();
	void dispose();
	void decodeFrame(const Frame &frame);
	void unfilter(uint8_t *row, const uint8_t *previous, size_t length) const;
	const uint8_t *expandRow(const uint8_t *row, int width);
	void compositeRow(const Frame &frame, int y, const uint8_t *pixels);
	void resample(const Rect &dirty);

	std::string data;
	const uint8_t *in = nullptr;
	size_t size = 0;

	int width = 0;
	int height = 0;
	int bitDepth = 0;
	int colorType = 0;
	int bitsPerPixel = 0;
	int targetW = 0;
	int targetH = 0;
	int p2w = 0;
	int p2h = 0;

	uint8_t palette[256][4] = {};
	int transparentKey[3] = {-1, -1, -1};

	std::vector<Frame> frames;
	size_t cursor = 0;
	bool needsBackup = false;
	int disposal = 0;
	Rect disposalRect;

	z_stream_s *stream = nullptr;
	std::vector<uint8_t> rows;
	std::vector<uint8_t> rgba;
	std::vector<uint8_t> canvas;
	std::vector<uint8_t> backup;
	std::vector<uint32_t> output;
	std::vector<int> columnStart;
	std::vector<int> rowStart;
};

} // namespace Apng
} // namespace Utils
