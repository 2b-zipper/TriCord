#ifndef UTF8_UTILS_H
#define UTF8_UTILS_H

#include <cstdint>
#include <string>
#include <vector>

namespace Utils {
namespace Utf8 {
uint32_t decodeNext(const std::string &text, size_t &cursor);

std::string codepointToHex(uint32_t cp);

bool isEmoji(uint32_t cp);

std::string sanitizeText(const std::string &text);
} // namespace Utf8
} // namespace Utils

#endif
