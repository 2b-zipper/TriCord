#include "utils/apng_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <zlib.h>

namespace Utils {
namespace Apng {

namespace {

constexpr uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr size_t SIGNATURE_SIZE = 8;
constexpr size_t CHUNK_HEADER_SIZE = 8;
constexpr size_t CHUNK_OVERHEAD = 12;
constexpr uint32_t IHDR_SIZE = 13;
constexpr uint32_t FCTL_SIZE = 26;
constexpr uint32_t SEQUENCE_SIZE = 4;

constexpr int COLOR_GRAY = 0;
constexpr int COLOR_RGB = 2;
constexpr int COLOR_PALETTE = 3;
constexpr int COLOR_GRAY_ALPHA = 4;
constexpr int COLOR_RGBA = 6;

constexpr int DISPOSE_BACKGROUND = 1;
constexpr int DISPOSE_PREVIOUS = 2;
constexpr int BLEND_OVER = 1;

constexpr int FILTER_SUB = 1;
constexpr int FILTER_UP = 2;
constexpr int FILTER_AVERAGE = 3;
constexpr int FILTER_PAETH = 4;

constexpr int MAX_SIDE = 1024;
constexpr int MIN_TEXTURE_SIDE = 8;
// Inflating many rows per call keeps zlib on its fast path, which needs 258 bytes of room.
constexpr size_t BATCH_BYTES = 16 * 1024;
constexpr size_t WORD_PADDING = 4;
// zlib's inflate state plus its 32KB window.
constexpr size_t INFLATE_BYTES = 40 * 1024;

// Browsers show a frame of 10ms or less for 100ms.
constexpr int MIN_DELAY_MS = 11;
constexpr int DEFAULT_DELAY_MS = 100;

constexpr int MORTON_X[8] = {0, 1, 4, 5, 16, 17, 20, 21};
constexpr int MORTON_Y[8] = {0, 2, 8, 10, 32, 34, 40, 42};

uint32_t readU32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int readU16(const uint8_t *p) { return p[0] << 8 | p[1]; }

bool chunkIs(const uint8_t *chunk, const char *type) { return memcmp(chunk + 4, type, 4) == 0; }

int pixelBits(int colorType, int depth) {
	const bool byteDepth = depth == 8 || depth == 16;
	switch (colorType) {
	case COLOR_GRAY:
		return byteDepth || depth == 1 || depth == 2 || depth == 4 ? depth : 0;
	case COLOR_RGB:
		return byteDepth ? depth * 3 : 0;
	case COLOR_PALETTE:
		return depth == 1 || depth == 2 || depth == 4 || depth == 8 ? depth : 0;
	case COLOR_GRAY_ALPHA:
		return byteDepth ? depth * 2 : 0;
	case COLOR_RGBA:
		return byteDepth ? depth * 4 : 0;
	}
	return 0;
}

template <typename Visit> void forEachChunk(const uint8_t *in, size_t size, Visit visit) {
	size_t position = SIGNATURE_SIZE;
	while (position + CHUNK_OVERHEAD <= size) {
		const uint32_t length = readU32(in + position);
		if (length > size - position - CHUNK_OVERHEAD || !visit(in + position, position + CHUNK_HEADER_SIZE, length)) {
			return;
		}
		position += CHUNK_OVERHEAD + length;
	}
}

typedef uint32_t __attribute__((may_alias)) Word;

void copyWords(uint8_t *dst, const uint8_t *src, size_t words) {
	const unsigned shift = ((uintptr_t)src & 3) * 8;
	if (shift == 0) {
		memcpy(dst, src, words * 4);
		return;
	}
	Word *out = reinterpret_cast<Word *>(dst);
	const Word *aligned = reinterpret_cast<const Word *>((uintptr_t)src & ~(uintptr_t)3);
	uint32_t low = *aligned++;
	for (size_t i = 0; i < words; i++) {
		const uint32_t high = *aligned++;
		out[i] = low >> shift | high << (32 - shift);
		low = high;
	}
}

uint8_t paeth(int a, int b, int c) {
	const int p = a + b - c;
	const int pa = abs(p - a);
	const int pb = abs(p - b);
	const int pc = abs(p - c);
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

} // namespace

bool isApng(const std::string &data) {
	const uint8_t *in = reinterpret_cast<const uint8_t *>(data.data());
	const size_t size = data.size();
	if (size < SIGNATURE_SIZE + CHUNK_OVERHEAD + IHDR_SIZE || memcmp(in, SIGNATURE, SIGNATURE_SIZE) != 0) {
		return false;
	}
	const uint8_t *header = in + SIGNATURE_SIZE;
	const uint8_t *fields = header + CHUNK_HEADER_SIZE;
	if (readU32(header) != IHDR_SIZE || !chunkIs(header, "IHDR") || pixelBits(fields[9], fields[8]) == 0 ||
	    fields[10] != 0 || fields[11] != 0 || fields[12] != 0) {
		return false;
	}

	bool animated = false;
	forEachChunk(in, size, [&](const uint8_t *chunk, size_t, uint32_t) {
		animated = chunkIs(chunk, "acTL");
		return !animated && !chunkIs(chunk, "IDAT");
	});
	return animated;
}

Decoder::Decoder(std::string source, int maxDim) : data(std::move(source)) {
	if (!isApng(data)) {
		return;
	}

	in = reinterpret_cast<const uint8_t *>(data.data());
	size = data.size();
	const uint8_t *fields = in + SIGNATURE_SIZE + CHUNK_HEADER_SIZE;
	const uint32_t w = readU32(fields);
	const uint32_t h = readU32(fields + 4);
	if (w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE) {
		return;
	}
	width = w;
	height = h;
	bitDepth = fields[8];
	colorType = fields[9];
	bitsPerPixel = pixelBits(colorType, bitDepth);
	for (auto &entry : palette) {
		entry[3] = 255;
	}

	if (!scan()) {
		return;
	}

	stream = new (std::nothrow) z_stream_s();
	if (!stream || inflateInit(stream) != Z_OK) {
		delete stream;
		stream = nullptr;
		return;
	}

	const size_t rowLength = ((size_t)width * bitsPerPixel + 7) / 8 + 1;
	rows.assign(rowLength + std::max(BATCH_BYTES, rowLength) + WORD_PADDING, 0);
	rgba.assign((size_t)width * 4, 0);
	canvas.assign((size_t)width * height * 4, 0);
	if (needsBackup) {
		backup.assign(canvas.size(), 0);
	}
	buildSampling(maxDim);
}

Decoder::~Decoder() {
	if (stream) {
		inflateEnd(stream);
		delete stream;
	}
}

size_t Decoder::memoryBytes() const {
	return data.size() + canvas.size() + backup.size() + rows.size() + rgba.size() + pixelBytes() + INFLATE_BYTES;
}

// An IDAT ahead of the first fcTL is the fallback still, not a frame.
bool Decoder::scan() {
	bool sawFrameData = false;
	forEachChunk(in, size, [&](const uint8_t *chunk, size_t offset, uint32_t length) {
		const uint8_t *body = in + offset;
		if (chunkIs(chunk, "PLTE")) {
			for (uint32_t i = 0; i < std::min<uint32_t>(length / 3, 256); i++) {
				memcpy(palette[i], body + i * 3, 3);
			}
		} else if (chunkIs(chunk, "tRNS")) {
			if (colorType == COLOR_PALETTE) {
				for (uint32_t i = 0; i < std::min<uint32_t>(length, 256); i++) {
					palette[i][3] = body[i];
				}
			} else if (colorType == COLOR_GRAY && length >= 2) {
				transparentKey[0] = readU16(body);
			} else if (colorType == COLOR_RGB && length >= 6) {
				for (int c = 0; c < 3; c++) {
					transparentKey[c] = readU16(body + c * 2);
				}
			}
		} else if (chunkIs(chunk, "fcTL")) {
			if (length < FCTL_SIZE) {
				return false;
			}
			const uint32_t w = readU32(body + 4);
			const uint32_t h = readU32(body + 8);
			const uint32_t left = readU32(body + 12);
			const uint32_t top = readU32(body + 16);
			if (w == 0 || h == 0 || left >= (uint32_t)width || top >= (uint32_t)height || w > width - left ||
			    h > height - top) {
				return false;
			}

			Frame frame;
			frame.rect = {(int)left, (int)top, (int)w, (int)h};
			const int numerator = readU16(body + 20);
			const int denominator = readU16(body + 22);
			const int delayMs = numerator * 1000 / (denominator ? denominator : 100);
			frame.delayMs = delayMs < MIN_DELAY_MS ? DEFAULT_DELAY_MS : delayMs;
			frame.dispose = body[24];
			frame.blend = body[25];
			if (frames.empty() && frame.dispose == DISPOSE_PREVIOUS) {
				frame.dispose = DISPOSE_BACKGROUND;
			}
			needsBackup |= frame.dispose == DISPOSE_PREVIOUS;
			frames.push_back(std::move(frame));
		} else if (chunkIs(chunk, "IDAT")) {
			if (frames.size() == 1 && !sawFrameData) {
				frames[0].chunks.push_back({offset, length});
			}
		} else if (chunkIs(chunk, "fdAT")) {
			sawFrameData = true;
			if (!frames.empty() && length > SEQUENCE_SIZE) {
				frames.back().chunks.push_back({offset + SEQUENCE_SIZE, length - SEQUENCE_SIZE});
			}
		} else if (chunkIs(chunk, "IEND")) {
			return false;
		}
		return true;
	});

	while (!frames.empty() && frames.back().chunks.empty()) {
		frames.pop_back();
	}
	return !frames.empty();
}

void Decoder::buildSampling(int maxDim) {
	targetW = width;
	targetH = height;
	if (targetW > maxDim || targetH > maxDim) {
		if (width >= height) {
			targetW = maxDim;
			targetH = std::max(1, maxDim * height / width);
		} else {
			targetH = maxDim;
			targetW = std::max(1, maxDim * width / height);
		}
	}

	p2w = MIN_TEXTURE_SIDE;
	p2h = MIN_TEXTURE_SIDE;
	while (p2w < targetW) {
		p2w *= 2;
	}
	while (p2h < targetH) {
		p2h *= 2;
	}

	output.assign((size_t)p2w * p2h, 0);
	columnStart.resize(targetW + 1);
	rowStart.resize(targetH + 1);
	for (int x = 0; x <= targetW; x++) {
		columnStart[x] = x * width / targetW;
	}
	for (int y = 0; y <= targetH; y++) {
		rowStart[y] = y * height / targetH;
	}
}

Image::TiledData Decoder::copyPixels() const {
	Image::TiledData tiled;
	tiled.pixels = (u32 *)malloc(pixelBytes());
	if (!tiled.pixels) {
		return tiled;
	}
	memcpy(tiled.pixels, pixels(), pixelBytes());
	tiled.w = targetW;
	tiled.h = targetH;
	tiled.p2w = p2w;
	tiled.p2h = p2h;
	tiled.vramSize = pixelBytes();
	tiled.format = GPU_RGBA8;
	return tiled;
}

void Decoder::rewind() {
	cursor = 0;
	disposal = 0;
	std::fill(canvas.begin(), canvas.end(), 0);
}

void Decoder::dispose() {
	if (disposal == DISPOSE_BACKGROUND || disposal == DISPOSE_PREVIOUS) {
		const size_t rowLength = (size_t)disposalRect.width * 4;
		for (int y = 0; y < disposalRect.height; y++) {
			uint8_t *row = canvas.data() + ((size_t)(disposalRect.top + y) * width + disposalRect.left) * 4;
			if (disposal == DISPOSE_BACKGROUND) {
				memset(row, 0, rowLength);
			} else {
				memcpy(row, backup.data() + y * rowLength, rowLength);
			}
		}
	}
	disposal = 0;
}

int Decoder::next() {
	Rect dirty = disposal ? disposalRect : Rect{};
	dispose();
	if (cursor >= frames.size()) {
		rewind();
		dirty = {0, 0, width, height};
	}

	const Frame &frame = frames[cursor++];
	const Rect &rect = frame.rect;
	if (frame.dispose == DISPOSE_PREVIOUS) {
		const size_t rowLength = (size_t)rect.width * 4;
		for (int y = 0; y < rect.height; y++) {
			memcpy(backup.data() + y * rowLength, canvas.data() + ((size_t)(rect.top + y) * width + rect.left) * 4,
			       rowLength);
		}
	}

	decodeFrame(frame);
	disposal = frame.dispose;
	disposalRect = rect;

	if (dirty.width == 0) {
		dirty = rect;
	} else {
		const int right = std::max(dirty.left + dirty.width, rect.left + rect.width);
		const int bottom = std::max(dirty.top + dirty.height, rect.top + rect.height);
		dirty.left = std::min(dirty.left, rect.left);
		dirty.top = std::min(dirty.top, rect.top);
		dirty.width = right - dirty.left;
		dirty.height = bottom - dirty.top;
	}
	resample(dirty);
	return frame.delayMs;
}

void Decoder::decodeFrame(const Frame &frame) {
	if (frame.chunks.empty() || inflateReset(stream) != Z_OK) {
		return;
	}

	const size_t rowLength = ((size_t)frame.rect.width * bitsPerPixel + 7) / 8 + 1;
	const int batchRows = (int)std::max<size_t>(1, BATCH_BYTES / rowLength);
	// zlib copies its output into its window with memcpy, which slows to bytes when the batch is unaligned.
	uint8_t *batch = rows.data();
	uint8_t *previous = rows.data() + rows.size() - rowLength;
	memset(previous, 0, rowLength);

	size_t chunk = 0;
	stream->next_in = const_cast<Bytef *>(in + frame.chunks[0].offset);
	stream->avail_in = frame.chunks[0].length;
	int y = 0;
	while (y < frame.rect.height) {
		const size_t wanted = rowLength * std::min(batchRows, frame.rect.height - y);
		size_t filled = 0;
		bool ended = false;
		while (filled < wanted) {
			stream->next_out = batch + filled;
			stream->avail_out = wanted - filled;
			const int status = inflate(stream, Z_NO_FLUSH);
			filled = wanted - stream->avail_out;
			if (status != Z_OK && status != Z_BUF_ERROR) {
				ended = true;
				break;
			}
			if (filled < wanted && stream->avail_in == 0) {
				if (++chunk == frame.chunks.size()) {
					ended = true;
					break;
				}
				stream->next_in = const_cast<Bytef *>(in + frame.chunks[chunk].offset);
				stream->avail_in = frame.chunks[chunk].length;
			}
		}

		const uint8_t *above = previous;
		uint8_t *row = batch;
		for (size_t done = rowLength; done <= filled; done += rowLength, row += rowLength) {
			unfilter(row, above, rowLength);
			compositeRow(frame, y++, expandRow(row + 1, frame.rect.width));
			above = row;
		}
		if (ended) {
			return;
		}
		memcpy(previous, above, rowLength);
	}
}

void Decoder::unfilter(uint8_t *row, const uint8_t *previous, size_t length) const {
	const size_t bpp = std::max(1, bitsPerPixel / 8);
	uint8_t *p = row + 1;
	const uint8_t *up = previous + 1;
	const size_t n = length - 1;
	switch (row[0]) {
	case FILTER_SUB:
		for (size_t i = bpp; i < n; i++) {
			p[i] += p[i - bpp];
		}
		break;
	case FILTER_UP:
		for (size_t i = 0; i < n; i++) {
			p[i] += up[i];
		}
		break;
	case FILTER_AVERAGE:
		for (size_t i = 0; i < std::min(bpp, n); i++) {
			p[i] += up[i] >> 1;
		}
		for (size_t i = bpp; i < n; i++) {
			p[i] += (p[i - bpp] + up[i]) >> 1;
		}
		break;
	case FILTER_PAETH:
		for (size_t i = 0; i < std::min(bpp, n); i++) {
			p[i] += up[i];
		}
		for (size_t i = bpp; i < n; i++) {
			p[i] += paeth(p[i - bpp], up[i], up[i - bpp]);
		}
		break;
	}
}

const uint8_t *Decoder::expandRow(const uint8_t *row, int count) {
	if (colorType == COLOR_RGBA && bitDepth == 8) {
		return row;
	}
	uint8_t *out = rgba.data();

	const int channels = bitsPerPixel / bitDepth;
	const int maxValue = (1 << bitDepth) - 1;
	const auto sample = [&](int index) -> int {
		if (bitDepth == 16) {
			return row[index * 2] << 8 | row[index * 2 + 1];
		}
		if (bitDepth == 8) {
			return row[index];
		}
		const int bit = index * bitDepth;
		return (row[bit >> 3] >> (8 - bitDepth - (bit & 7))) & maxValue;
	};
	const auto to8 = [&](int value) -> uint8_t {
		return bitDepth == 16 ? value >> 8 : bitDepth == 8 ? value : value * 255 / maxValue;
	};

	for (int x = 0; x < count; x++, out += 4) {
		const int base = x * channels;
		switch (colorType) {
		case COLOR_GRAY: {
			const int value = sample(base);
			out[0] = out[1] = out[2] = to8(value);
			out[3] = value == transparentKey[0] ? 0 : 255;
			break;
		}
		case COLOR_RGB: {
			bool keyed = true;
			for (int c = 0; c < 3; c++) {
				const int value = sample(base + c);
				out[c] = to8(value);
				keyed &= value == transparentKey[c];
			}
			out[3] = keyed ? 0 : 255;
			break;
		}
		case COLOR_PALETTE:
			memcpy(out, palette[sample(base)], 4);
			break;
		case COLOR_GRAY_ALPHA:
			out[0] = out[1] = out[2] = to8(sample(base));
			out[3] = to8(sample(base + 1));
			break;
		default:
			for (int c = 0; c < 4; c++) {
				out[c] = to8(sample(base + c));
			}
			break;
		}
	}
	return rgba.data();
}

void Decoder::compositeRow(const Frame &frame, int y, const uint8_t *pixels) {
	uint8_t *dst = canvas.data() + ((size_t)(frame.rect.top + y) * width + frame.rect.left) * 4;
	const uint8_t *src = pixels;
	if (frame.blend != BLEND_OVER) {
		copyWords(dst, src, frame.rect.width);
		return;
	}

	for (int x = 0; x < frame.rect.width; x++, src += 4, dst += 4) {
		const int sourceAlpha = src[3];
		if (sourceAlpha == 255) {
			memcpy(dst, src, 4);
			continue;
		}
		if (sourceAlpha == 0) {
			continue;
		}
		const int below = (dst[3] * (255 - sourceAlpha) + 127) / 255;
		const int alpha = sourceAlpha + below;
		for (int c = 0; c < 3; c++) {
			dst[c] = (src[c] * sourceAlpha + dst[c] * below + alpha / 2) / alpha;
		}
		dst[3] = alpha;
	}
}

// Colors are averaged by alpha, so transparent pixels do not darken the edges.
void Decoder::resample(const Rect &dirty) {
	const int tilesPerRow = p2w >> 3;
	const int right = dirty.left + dirty.width;
	const int bottom = dirty.top + dirty.height;
	for (int ty = 0; ty < targetH; ty++) {
		const int y0 = rowStart[ty];
		const int y1 = std::max(rowStart[ty + 1], y0 + 1);
		if (y1 <= dirty.top || y0 >= bottom) {
			continue;
		}
		uint32_t *tileRow = output.data() + ((size_t)(ty >> 3) * tilesPerRow << 6) + MORTON_Y[ty & 7];
		for (int tx = 0; tx < targetW; tx++) {
			const int x0 = columnStart[tx];
			const int x1 = std::max(columnStart[tx + 1], x0 + 1);
			if (x1 <= dirty.left || x0 >= right) {
				continue;
			}
			uint32_t r = 0, g = 0, b = 0, a = 0;
			for (int sy = y0; sy < y1; sy++) {
				const uint8_t *p = canvas.data() + ((size_t)sy * width + x0) * 4;
				for (int sx = x0; sx < x1; sx++, p += 4) {
					r += p[0] * p[3];
					g += p[1] * p[3];
					b += p[2] * p[3];
					a += p[3];
				}
			}
			const uint32_t n = (uint32_t)((y1 - y0) * (x1 - x0));
			const uint32_t alpha = (a + n / 2) / n;
			uint32_t texel = 0;
			if (alpha) {
				// ceil(2^32 / a) gives exact quotients while a stays below 4100, which covers boxes up to 4x4.
				const uint64_t inverse = a > 1 ? 0xFFFFFFFFu / a + 1 : 1ull << 32;
				texel = (uint32_t)(r * inverse >> 32) << 24 | (uint32_t)(g * inverse >> 32) << 16 |
				        (uint32_t)(b * inverse >> 32) << 8 | alpha;
			}
			tileRow[((tx >> 3) << 6) + MORTON_X[tx & 7]] = texel;
		}
	}
}

} // namespace Apng
} // namespace Utils
