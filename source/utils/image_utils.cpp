#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "utils/image_utils.h"
#include <cmath>
#include <malloc.h>
#include <string.h>

namespace Utils {
namespace Image {

static const int mortonTable[] = {
    0,  1,  4,  5,  16, 17, 20, 21, 2,  3,  6,  7,  18, 19, 22, 23,
    8,  9,  12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63};

TiledData decodeToTiled(const unsigned char *data, size_t size, int maxWidth,
                        int maxHeight, bool noResize) {
  TiledData result;
  int w, h, c;

  if (!stbi_info_from_memory(data, size, &w, &h, &c)) {
    return result;
  }

  if (w > 8192 || h > 8192)
    return result;
  if (w * h > 3000 * 3000)
    return result;

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

  int p2_w = 1, p2_h = 1;
  while (p2_w < targetW)
    p2_w *= 2;
  while (p2_h < targetH)
    p2_h *= 2;

  size_t vramSize = p2_w * p2_h * 4;
  u32 *tiledBuf = (u32 *)malloc(vramSize);
  if (!tiledBuf) {
    stbi_image_free(img);
    return result;
  }
  memset(tiledBuf, 0, vramSize);

  for (int y = 0; y < targetH; y++) {
    for (int x = 0; x < targetW; x++) {
      int sx = (x * w) / targetW;
      int sy = (y * h) / targetH;

      int srcIdx = (sy * w + sx) * 4;
      u8 r = img[srcIdx + 0];
      u8 g = img[srcIdx + 1];
      u8 b = img[srcIdx + 2];
      u8 a = img[srcIdx + 3];

      u32 color = (r << 24) | (g << 16) | (b << 8) | a;

      int tileX = x & 7;
      int tileY = y & 7;
      int tileIdx = ((y >> 3) * (p2_w >> 3) + (x >> 3)) * 64;
      int dstIdx = tileIdx + mortonTable[tileY * 8 + tileX];

      if (dstIdx < (int)(vramSize / 4)) {
        tiledBuf[dstIdx] = color;
      }
    }
  }

  stbi_image_free(img);

  result.pixels = tiledBuf;
  result.w = targetW;
  result.h = targetH;
  result.p2w = p2_w;
  result.p2h = p2_h;
  result.vramSize = vramSize;
  return result;
}

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size,
                               int &outW, int &outH, bool noResize) {
  TiledData tiled = decodeToTiled(data, size, 512, 512, noResize);
  if (!tiled.pixels)
    return nullptr;

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
