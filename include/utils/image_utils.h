#pragma once
#include <citro3d.h>
#include <cstdio>

namespace Utils {
namespace Image {

struct TiledData {
  u32 *pixels = nullptr;
  int w = 0, h = 0;
  int p2w = 0, p2h = 0;
  size_t vramSize = 0;
};

TiledData decodeToTiled(const unsigned char *data, size_t size,
                        int maxWidth = 512, int maxHeight = 512,
                        bool noResize = false);

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size,
                               int &outW, int &outH, bool noResize = false);

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size);

} // namespace Image
} // namespace Utils
