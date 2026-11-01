#include "utils/gif_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <strings.h>

namespace Utils {
namespace Gif {

namespace {

constexpr size_t HEADER_SIZE = 13;
constexpr size_t IMAGE_DESCRIPTOR_SIZE = 9;
constexpr uint8_t BLOCK_EXTENSION = 0x21;
constexpr uint8_t BLOCK_IMAGE = 0x2C;
constexpr uint8_t EXTENSION_GRAPHIC_CONTROL = 0xF9;
constexpr uint8_t GRAPHIC_CONTROL_SIZE = 4;

constexpr uint8_t FLAG_PALETTE = 0x80;
constexpr uint8_t FLAG_INTERLACED = 0x40;
constexpr int DISPOSE_BACKGROUND = 2;
constexpr int DISPOSE_PREVIOUS = 3;

constexpr int MAX_CODES = 4096;
constexpr int MAX_MIN_CODE_SIZE = 11;
constexpr int MAX_SIDE = 8192;
constexpr int MAX_PIXELS = 3000 * 3000;
constexpr int MIN_TEXTURE_SIDE = 8;

// Browsers show anything faster than 20ms at the default rate, and GIFs are authored against that.
constexpr int MIN_DELAY_MS = 20;

constexpr int MORTON_X[8] = {0, 1, 4, 5, 16, 17, 20, 21};
constexpr int MORTON_Y[8] = {0, 2, 8, 10, 32, 34, 40, 42};

constexpr int INTERLACE_START[4] = {0, 4, 2, 1};
constexpr int INTERLACE_STEP[4] = {8, 8, 4, 2};

struct LzwTables {
	uint16_t prefix[MAX_CODES];
	uint8_t suffix[MAX_CODES];
	uint8_t stack[MAX_CODES + 1];
};

int readU16(const uint8_t *p) { return p[0] | (p[1] << 8); }

int paletteEntries(uint8_t flags) { return 2 << (flags & 7); }

void loadPalette(uint16_t *palette, const uint8_t *rgb, int entries) {
	for (int i = 0; i < entries; i++, rgb += 3) {
		palette[i] = ((rgb[0] & 0xF8) << 8) | ((rgb[1] & 0xF8) << 3) | ((rgb[2] & 0xF8) >> 2) | 1;
	}
	std::fill(palette + entries, palette + 256, 0);
}

} // namespace

bool isGif(const std::string &data) { return data.size() >= HEADER_SIZE && data.compare(0, 4, "GIF8") == 0; }

bool hasGifPath(const std::string &url) {
	const size_t end = std::min(url.find('?'), url.size());
	return end >= 4 && strncasecmp(url.c_str() + end - 4, ".gif", 4) == 0;
}

Decoder::Decoder(std::string source, int maxDim) : data(std::move(source)) {
	if (!isGif(data)) {
		return;
	}

	in = reinterpret_cast<const uint8_t *>(data.data());
	size = data.size();
	screenW = readU16(in + 6);
	screenH = readU16(in + 8);

	const uint8_t flags = in[10];
	firstBlock = HEADER_SIZE;
	if (flags & FLAG_PALETTE) {
		const int entries = paletteEntries(flags);
		if (firstBlock + entries * 3 > size) {
			return;
		}
		loadPalette(globalPalette, in + firstBlock, entries);
		firstBlock += entries * 3;
	}

	if (scan()) {
		buildSampling(maxDim);
		cursor = firstBlock;
	}
}

// Some encoders place frames outside the logical screen, which browsers answer
// by growing the screen.
bool Decoder::scan() {
	size_t position = firstBlock;
	while (position < size) {
		const uint8_t block = in[position++];
		if (block == BLOCK_EXTENSION) {
			position++;
			skipSubBlocks(position);
		} else if (block == BLOCK_IMAGE) {
			if (position + IMAGE_DESCRIPTOR_SIZE >= size) {
				break;
			}
			screenW = std::max(screenW, readU16(in + position) + readU16(in + position + 4));
			screenH = std::max(screenH, readU16(in + position + 2) + readU16(in + position + 6));
			const uint8_t flags = in[position + 8];
			position += IMAGE_DESCRIPTOR_SIZE;
			if (flags & FLAG_PALETTE) {
				position += paletteEntries(flags) * 3;
			}
			position++;
			skipSubBlocks(position);
			frames++;
		} else {
			break;
		}
	}

	return frames > 0 && screenW > 0 && screenH > 0 && screenW <= MAX_SIDE && screenH <= MAX_SIDE &&
	       screenW * screenH <= MAX_PIXELS;
}

void Decoder::buildSampling(int maxDim) {
	targetW = screenW;
	targetH = screenH;
	if (targetW > maxDim || targetH > maxDim) {
		if (screenW >= screenH) {
			targetW = maxDim;
			targetH = std::max(1, maxDim * screenH / screenW);
		} else {
			targetH = maxDim;
			targetW = std::max(1, maxDim * screenW / screenH);
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

	canvas.assign((size_t)p2w * p2h, 0);
	columns.assign(screenW, -1);
	rows.assign(screenH, -1);

	const int tilesPerRow = p2w >> 3;
	for (int x = 0; x < targetW; x++) {
		columns[x * screenW / targetW] = ((x >> 3) << 6) + MORTON_X[x & 7];
	}
	for (int y = 0; y < targetH; y++) {
		rows[y * screenH / targetH] = (((y >> 3) * tilesPerRow) << 6) + MORTON_Y[y & 7];
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
	tiled.format = GPU_RGBA5551;
	return tiled;
}

void Decoder::skipSubBlocks(size_t &position) const {
	while (position < size) {
		const uint8_t length = in[position++];
		if (length == 0) {
			return;
		}
		position += length;
	}
}

uint16_t *Decoder::rowAt(int y) { return rows[y] >= 0 ? canvas.data() + rows[y] : nullptr; }

void Decoder::rewind() {
	cursor = firstBlock;
	disposal = 0;
	std::fill(canvas.begin(), canvas.end(), 0);
}

void Decoder::dispose() {
	if (disposal == DISPOSE_BACKGROUND) {
		for (int y = disposalRect.top; y < disposalRect.top + disposalRect.height; y++) {
			uint16_t *row = rowAt(y);
			if (!row) {
				continue;
			}
			for (int x = disposalRect.left; x < disposalRect.left + disposalRect.width; x++) {
				if (columns[x] >= 0) {
					row[columns[x]] = 0;
				}
			}
		}
	} else if (disposal == DISPOSE_PREVIOUS) {
		canvas = backup;
	}
	disposal = 0;
}

void Decoder::readExtension(GraphicControl &control) {
	if (cursor >= size) {
		return;
	}
	const uint8_t label = in[cursor++];
	if (label == EXTENSION_GRAPHIC_CONTROL && cursor + 5 < size && in[cursor] == GRAPHIC_CONTROL_SIZE) {
		const uint8_t flags = in[cursor + 1];
		const int delayMs = readU16(in + cursor + 2) * 10;
		control.disposal = (flags >> 2) & 7;
		control.delayMs = delayMs < MIN_DELAY_MS ? DEFAULT_DELAY_MS : delayMs;
		control.transparent = (flags & 1) ? in[cursor + 4] : -1;
	}
	skipSubBlocks(cursor);
}

int Decoder::next() {
	dispose();

	GraphicControl control;
	while (true) {
		const uint8_t block = cursor < size ? in[cursor++] : 0;
		if (block == BLOCK_IMAGE) {
			decodeImage(control);
			return control.delayMs;
		}
		if (block == BLOCK_EXTENSION) {
			readExtension(control);
		} else {
			rewind();
		}
	}
}

void Decoder::decodeImage(const GraphicControl &control) {
	if (cursor + IMAGE_DESCRIPTOR_SIZE >= size) {
		cursor = size;
		return;
	}

	Rect rect;
	rect.left = readU16(in + cursor);
	rect.top = readU16(in + cursor + 2);
	rect.width = readU16(in + cursor + 4);
	rect.height = readU16(in + cursor + 6);
	const uint8_t flags = in[cursor + 8];
	cursor += IMAGE_DESCRIPTOR_SIZE;

	const uint16_t *palette = globalPalette;
	if (flags & FLAG_PALETTE) {
		const int entries = paletteEntries(flags);
		if (cursor + entries * 3 >= size) {
			cursor = size;
			return;
		}
		loadPalette(localPalette, in + cursor, entries);
		cursor += entries * 3;
		palette = localPalette;
	}

	if (control.disposal == DISPOSE_PREVIOUS) {
		backup = canvas;
	}
	disposal = control.disposal;
	disposalRect = rect;

	decodeRaster(rect, palette, control.transparent, flags & FLAG_INTERLACED);
}

void Decoder::decodeRaster(const Rect &rect, const uint16_t *palette, int transparent, bool interlaced) {
	const int minCodeSize = in[cursor++];
	std::unique_ptr<LzwTables> tables(new (std::nothrow) LzwTables);
	if (!tables || minCodeSize < 1 || minCodeSize > MAX_MIN_CODE_SIZE) {
		skipSubBlocks(cursor);
		return;
	}

	uint16_t *prefix = tables->prefix;
	uint8_t *suffix = tables->suffix;
	uint8_t *stack = tables->stack;

	const int clearCode = 1 << minCodeSize;
	const int endCode = clearCode + 1;
	for (int i = 0; i < clearCode; i++) {
		suffix[i] = i;
	}

	int available = endCode + 1;
	int codeSize = minCodeSize + 1;
	int codeMask = (1 << codeSize) - 1;
	int previous = -1;
	uint8_t first = 0;

	uint32_t bits = 0;
	int bitCount = 0;
	size_t blockLeft = 0;
	bool blocksEnded = false;

	const int right = rect.left + rect.width;
	int x = rect.left;
	int y = 0;
	int pass = 0;
	bool rasterDone = rect.width == 0 || rect.height == 0;
	uint16_t *row = rasterDone ? nullptr : rowAt(rect.top);

	while (!rasterDone) {
		while (bitCount < codeSize) {
			if (blockLeft == 0) {
				blockLeft = cursor < size ? in[cursor++] : 0;
				if (blockLeft == 0) {
					blocksEnded = true;
					break;
				}
			}
			if (cursor >= size) {
				blocksEnded = true;
				break;
			}
			bits |= (uint32_t)in[cursor++] << bitCount;
			bitCount += 8;
			blockLeft--;
		}
		if (blocksEnded) {
			break;
		}

		const int code = bits & codeMask;
		bits >>= codeSize;
		bitCount -= codeSize;

		if (code == clearCode) {
			available = endCode + 1;
			codeSize = minCodeSize + 1;
			codeMask = (1 << codeSize) - 1;
			previous = -1;
			continue;
		}
		if (code == endCode) {
			break;
		}

		int length = 0;
		if (previous < 0) {
			if (code >= clearCode) {
				break;
			}
			first = code;
			stack[length++] = first;
		} else {
			if (code > available) {
				break;
			}
			int current = code;
			if (code == available) {
				stack[length++] = first;
				current = previous;
			}
			while (current >= clearCode) {
				stack[length++] = suffix[current];
				current = prefix[current];
			}
			first = current;
			stack[length++] = first;

			if (available < MAX_CODES) {
				prefix[available] = previous;
				suffix[available] = first;
				available++;
				if ((available & codeMask) == 0 && available < MAX_CODES) {
					codeSize++;
					codeMask = (1 << codeSize) - 1;
				}
			}
		}
		previous = code;

		while (length > 0) {
			const int index = stack[--length];
			if (row && index != transparent && columns[x] >= 0) {
				row[columns[x]] = palette[index];
			}

			if (++x < right) {
				continue;
			}
			x = rect.left;
			if (interlaced) {
				y += INTERLACE_STEP[pass];
				while (y >= rect.height && pass < 3) {
					y = INTERLACE_START[++pass];
				}
			} else {
				y++;
			}
			if (y >= rect.height) {
				rasterDone = true;
				break;
			}
			row = rowAt(rect.top + y);
		}
	}

	if (!blocksEnded) {
		cursor += blockLeft;
		skipSubBlocks(cursor);
	}
}

} // namespace Gif
} // namespace Utils
