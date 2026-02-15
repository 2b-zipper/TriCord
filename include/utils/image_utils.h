#pragma once
#include <citro3d.h>
#include <cstdio>

namespace Utils {
namespace Image {

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size,
                               int &outW, int &outH, bool noResize = false);

C3D_Tex *loadTextureFromMemory(const unsigned char *data, size_t size);

} // namespace Image
} // namespace Utils
