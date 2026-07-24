#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "utils/image_utils.h"
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <vector>

namespace Utils {
namespace Image {

static const int mortonTable[] = {0,  1,  4,  5,  16, 17, 20, 21, 2,  3,  6,  7,  18, 19, 22, 23,
                                  8,  9,  12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
                                  32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51, 54, 55,
                                  40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63};

TiledData decodeToTiled(const unsigned char *data, size_t size, int maxWidth, int maxHeight, bool noResize,
                        float cornerRatio) {
	TiledData result;
	int w, h, c;

	if (!stbi_info_from_memory(data, size, &w, &h, &c)) {
		return result;
	}

	if (w > 8192 || h > 8192) {
		return result;
	}
	if (w * h > 3000 * 3000) {
		return result;
	}

	stbi_set_flip_vertically_on_load(false);
	unsigned char *img = stbi_load_from_memory(data, size, &w, &h, &c, 4);

	if (!img) {
		return result;
	}

	int targetW = w;
	int targetH = h;
	if (!noResize && (targetW > maxWidth || targetH > maxHeight)) {
		float ratio = (float)w / h;
		if (w > h) {
			targetW = maxWidth;
			targetH = maxWidth / ratio;
		} else {
			targetH = maxHeight;
			targetW = maxHeight * ratio;
		}
	}
	if (targetW < 1) {
		targetW = 1;
	}
	if (targetH < 1) {
		targetH = 1;
	}

	int p2_w = 1, p2_h = 1;
	while (p2_w < targetW) {
		p2_w *= 2;
	}
	while (p2_h < targetH) {
		p2_h *= 2;
	}

	size_t vramSize = (size_t)p2_w * p2_h * 4;
	u32 *tiledBuf = (u32 *)malloc(vramSize);
	if (!tiledBuf) {
		stbi_image_free(img);
		return result;
	}

	// The loop below writes every pixel of the target rect, so only padding needs clearing.
	if (p2_w != targetW || p2_h != targetH) {
		memset(tiledBuf, 0, vramSize);
	}

	std::vector<int> colMap(targetW);
	std::vector<int> rowMap(targetH);
	for (int x = 0; x < targetW; x++) {
		colMap[x] = (int)(((int64_t)x * w) / targetW);
	}
	for (int y = 0; y < targetH; y++) {
		rowMap[y] = (int)(((int64_t)y * h) / targetH);
	}

	const u32 *src = (const u32 *)img;
	const int tilesPerRow = p2_w >> 3;

	for (int y = 0; y < targetH; y++) {
		const u32 *srcRow = src + (size_t)rowMap[y] * w;
		const int *morton = &mortonTable[(y & 7) << 3];
		u32 *tileRow = tiledBuf + ((size_t)(y >> 3) * tilesPerRow << 6);

		for (int x = 0; x < targetW; x++) {
			// stb writes R,G,B,A; the GPU wants the reverse.
			tileRow[((x >> 3) << 6) + morton[x & 7]] = __builtin_bswap32(srcRow[colMap[x]]);
		}
	}

	stbi_image_free(img);

	// Bytes are already swapped to RRGGBBAA, so alpha is the low byte.
	// Rounded-rect mask. A ratio of 0.5 makes the radius half the shorter side,
	// which is a circle; smaller ratios give a squircle.
	if (cornerRatio > 0.0f) {
		const float cx = (targetW - 1) * 0.5f;
		const float cy = (targetH - 1) * 0.5f;
		const float hx = cx + 0.5f;
		const float hy = cy + 0.5f;

		float radius = cornerRatio * (targetW < targetH ? targetW : targetH);
		if (radius > hx) {
			radius = hx;
		}
		if (radius > hy) {
			radius = hy;
		}

		for (int y = 0; y < targetH; y++) {
			const int *morton = &mortonTable[(y & 7) << 3];
			u32 *tileRow = tiledBuf + ((size_t)(y >> 3) * tilesPerRow << 6);
			const float qy = fabsf(y - cy) - (hy - radius);

			for (int x = 0; x < targetW; x++) {
				const float qx = fabsf(x - cx) - (hx - radius);
				const float mx = qx > 0.0f ? qx : 0.0f;
				const float my = qy > 0.0f ? qy : 0.0f;
				float inner = qx > qy ? qx : qy;
				if (inner > 0.0f) {
					inner = 0.0f;
				}
				float coverage = radius - (sqrtf(mx * mx + my * my) + inner);
				if (coverage >= 1.0f) {
					continue;
				}

				u32 *px = &tileRow[((x >> 3) << 6) + morton[x & 7]];
				if (coverage <= 0.0f) {
					*px &= 0xFFFFFF00u;
					continue;
				}
				*px = (*px & 0xFFFFFF00u) | (u32)((*px & 0xFFu) * coverage);
			}
		}
	}

	result.pixels = tiledBuf;
	result.w = targetW;
	result.h = targetH;
	result.p2w = p2_w;
	result.p2h = p2_h;
	result.vramSize = vramSize;
	return result;
}

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size, int &outW, int &outH, bool noResize) {
	TiledData tiled = decodeToTiled(data, size, MAX_REMOTE_DIM, MAX_REMOTE_DIM, noResize);
	if (!tiled.pixels) {
		return nullptr;
	}

	C3D_Tex *tex = (C3D_Tex *)malloc(sizeof(C3D_Tex));
	if (!C3D_TexInit(tex, tiled.p2w, tiled.p2h, GPU_RGBA8)) {
		free(tiled.pixels);
		free(tex);
		return nullptr;
	}

	C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
	memcpy(tex->data, tiled.pixels, tiled.vramSize);
	GSPGPU_FlushDataCache(tex->data, tex->size);
	free(tiled.pixels);

	outW = tiled.w;
	outH = tiled.h;
	return tex;
}

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size) {
	int w, h;
	return loadTextureFromMemory(data, size, w, h, true);
}

} // namespace Image
} // namespace Utils
