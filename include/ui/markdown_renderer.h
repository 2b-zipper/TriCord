#pragma once

#include "utils/markdown.h"
#include <citro2d.h>
#include <string>
#include <vector>

namespace UI {
namespace MarkdownRenderer {

struct Piece {
	std::string text;
	uint16_t style = 0;
	std::string url;
	float x = 0.0f;
	float width = 0.0f;
	float scale = 0.4f;
	uint32_t color = 0;
	uint32_t bgColor = 0;
};

struct Line {
	std::vector<Piece> pieces;
	Utils::Markdown::BlockType type = Utils::Markdown::BlockType::PARAGRAPH;
	float height = 12.0f;
	float indent = 0.0f;
	float scale = 0.4f;
	std::string bullet;
	bool blockStart = false;
	bool blockEnd = false;
};

struct Layout {
	std::vector<Line> lines;
	float height = 0.0f;
	float lastLineEndX = 0.0f;
	float lastLineHeight = 12.0f;
	Utils::Markdown::BlockType lastLineType = Utils::Markdown::BlockType::PARAGRAPH;
	bool hasSpoiler = false;
};

constexpr uint16_t EMBED_STYLES = 0xFFFF;

// Cached; the reference stays valid until the next call with a different key.
const Layout &get(const std::string &content, float maxWidth, float baseScale, float lineHeightRatio = 30.0f,
                  uint16_t allowedStyles = 0xFFFF, bool allowBlocks = true);
void draw(const Layout &layout, float x, float y, float z, u32 color, size_t maxLines = (size_t)-1,
          bool revealSpoilers = false);
float heightOf(const Layout &layout, size_t maxLines);
void clearCache();

} // namespace MarkdownRenderer
} // namespace UI
