#pragma once
#include <citro2d.h>
#include <string>


namespace Utils {
namespace Color {

u32 hexToColor(const std::string &hex);
std::string colorToHex(u32 color);

} // namespace Color
} // namespace Utils
