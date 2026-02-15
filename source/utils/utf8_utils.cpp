#include "utils/utf8_utils.h"
#include <iomanip>
#include <sstream>

namespace Utils {
namespace Utf8 {

uint32_t decodeNext(const std::string &text, size_t &cursor) {
  if (cursor >= text.length())
    return 0;

  unsigned char c = static_cast<unsigned char>(text[cursor]);
  uint32_t codepoint = 0;
  int bytes = 0;

  if ((c & 0xF8) == 0xF0) {
    codepoint = c & 0x07;
    bytes = 4;
  } else if ((c & 0xF0) == 0xE0) {
    codepoint = c & 0x0F;
    bytes = 3;
  } else if ((c & 0xE0) == 0xC0) {
    codepoint = c & 0x1F;
    bytes = 2;
  } else if (c < 0x80) {
    codepoint = c;
    bytes = 1;
  } else {
    cursor++;
    return 0xFFFD;
  }

  if (cursor + bytes > text.length()) {
    cursor = text.length();
    return 0xFFFD;
  }

  for (int i = 1; i < bytes; i++) {
    codepoint = (codepoint << 6) |
                (static_cast<unsigned char>(text[cursor + i]) & 0x3F);
  }

  cursor += bytes;
  return codepoint;
}

std::string codepointToHex(uint32_t cp) {
  std::stringstream ss;
  ss << std::hex << cp;
  return ss.str();
}

bool isEmoji(uint32_t cp) {
  struct Range {
    uint32_t start;
    uint32_t end;
  };

  static const Range ranges[] = {
      {0x203C, 0x203C},   // ‼️
      {0x2049, 0x2049},   // ⁉️
      {0x2122, 0x2122},   // ™️
      {0x2139, 0x2139},   // ℹ️
      {0x231A, 0x23F3},   // Misc Technical
      {0x24C2, 0x24C2},   // Ⓜ️
      {0x25AA, 0x25FE},   // Geometric Shapes
      {0x2600, 0x26FF},   // Misc Symbols
      {0x2700, 0x27BF},   // Dingbats
      {0x2934, 0x2935},   // ⤴️, ⤵️
      {0x2B05, 0x2B07},   // ⬅️, ⬆️, ⬇️
      {0x2B1B, 0x2B1C},   // ⬛, ⬜
      {0x2B50, 0x2B50},   // ⭐
      {0x2B55, 0x2B55},   // ⭕
      {0x3030, 0x3030},   // 〰️
      {0x303D, 0x303D},   // 〽️
      {0x3297, 0x3297},   // ㊗️
      {0x3299, 0x3299},   // ㊙️
      {0x1F1E6, 0x1F1FF}, // Flags
      {0x1F300, 0x1F5FF}, // Misc Symbols and Pictographs
      {0x1F600, 0x1F64F}, // Emoticons
      {0x1F680, 0x1F6FF}, // Transport and Map
      {0x1F7E0, 0x1F7EB}, // Large Circles/Squares
      {0x1F900, 0x1F9FF}, // Supplemental
      {0x1FA70, 0x1FAFF}  // Extended-A
  };

  for (const auto &range : ranges) {
    if (cp >= range.start && cp <= range.end) {
      return true;
    }
  }
  return false;
}

std::string removeMarkdown(const std::string &text) {
  std::string result = text;
  size_t pos = 0;

  while ((pos = result.find("**", pos)) != std::string::npos) {
    result.erase(pos, 2);
  }

  while ((pos = result.find("__", pos)) != std::string::npos) {
    result.erase(pos, 2);
  }

  while ((pos = result.find("~~", pos)) != std::string::npos) {
    result.erase(pos, 2);
  }

  while ((pos = result.find("||", pos)) != std::string::npos) {
    result.erase(pos, 2);
  }

  while ((pos = result.find("`", pos)) != std::string::npos) {
    result.erase(pos, 1);
  }

  return result;
}

std::string sanitizeText(const std::string &text) {
  std::string sanitized = text;

  size_t pos = 0;

  // Replace Wave Dash (U+301C) with Fullwidth Tilde (U+FF5E) for 3DS fonts
  while ((pos = sanitized.find("\xE3\x80\x9C", pos)) != std::string::npos) {
    sanitized.replace(pos, 3, "\xEF\xBD\x9E");
    pos += 3;
  }

  pos = 0;
  while ((pos = sanitized.find('$', pos)) != std::string::npos) {
    sanitized.replace(pos, 1, "$$");
    pos += 2;
  }

  return sanitized;
}

} // namespace Utf8
} // namespace Utils
