#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Utils {
namespace Markdown {

enum StyleFlag : uint16_t {
	STYLE_NONE = 0,
	STYLE_BOLD = 1 << 0,
	STYLE_ITALIC = 1 << 1,
	STYLE_UNDERLINE = 1 << 2,
	STYLE_STRIKE = 1 << 3,
	STYLE_CODE = 1 << 4,
	STYLE_SPOILER = 1 << 5,
	STYLE_LINK = 1 << 6,
	STYLE_MENTION = 1 << 7,
};

enum class BlockType {
	PARAGRAPH,
	HEADER1,
	HEADER2,
	HEADER3,
	SUBTEXT,
	QUOTE,
	LIST,
	CODE_BLOCK,
};

struct Span {
	std::string text;
	uint16_t style = STYLE_NONE;
	std::string url;
	uint32_t color = 0;
	uint32_t bgColor = 0;
};

struct Block {
	BlockType type = BlockType::PARAGRAPH;
	std::vector<Span> spans;
	int listDepth = 0;
	int listNumber = 0;
	bool ordered = false;
	std::string language;
};

// Disallowed syntax stays verbatim instead of being consumed.
struct Options {
	uint16_t allowedStyles = 0xFFFF;
	bool blocks = true;
};

std::vector<Block> parse(const std::string &text, const Options &options = Options());

std::string stripFormatting(const std::string &text);

} // namespace Markdown
} // namespace Utils
