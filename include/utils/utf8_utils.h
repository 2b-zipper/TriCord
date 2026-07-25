#ifndef UTF8_UTILS_H
#define UTF8_UTILS_H

#include <cstdint>
#include <string>

namespace Utils {
namespace Utf8 {
uint32_t decodeNext(const std::string &text, size_t &cursor);
std::string encode(uint32_t cp);

std::string codepointToHex(uint32_t cp);
std::string hexToUtf8(const std::string &hex);
std::string utf8ToHex(const std::string &utf8);

bool isEmoji(uint32_t cp);
bool isEmojiModifier(uint32_t cp);
bool isEmojiJoiner(uint32_t cp);

std::string getEmojiSequence(const std::string &text, size_t &cursor);

std::string getFirstChar(const std::string &text);
std::string sanitizeText(const std::string &text);
} // namespace Utf8
} // namespace Utils

#endif
