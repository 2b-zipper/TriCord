#include "ui/markdown_renderer.h"
#include "core/config.h"
#include "ui/screen_manager.h"
#include "utils/utf8_utils.h"

#include <algorithm>
#include <list>
#include <map>

namespace UI {
namespace MarkdownRenderer {

using namespace Utils::Markdown;

namespace {

constexpr size_t MAX_CACHE_ENTRIES = 128;

struct CacheKey {
	std::string content;
	float maxWidth;
	float scale;
	float ratio;
	uint16_t allowed;
	bool blocks;

	bool operator==(const CacheKey &o) const {
		return maxWidth == o.maxWidth && scale == o.scale && ratio == o.ratio && allowed == o.allowed &&
		       blocks == o.blocks && content == o.content;
	}

	bool operator<(const CacheKey &o) const {
		if (maxWidth != o.maxWidth) {
			return maxWidth < o.maxWidth;
		}
		if (scale != o.scale) {
			return scale < o.scale;
		}
		if (ratio != o.ratio) {
			return ratio < o.ratio;
		}
		if (allowed != o.allowed) {
			return allowed < o.allowed;
		}
		if (blocks != o.blocks) {
			return blocks < o.blocks;
		}
		return content < o.content;
	}
};

std::map<CacheKey, Layout> cache;
std::list<CacheKey> lru;

float blockScale(BlockType t, float base) {
	switch (t) {
	case BlockType::HEADER1:
		return base * 1.6f;
	case BlockType::HEADER2:
		return base * 1.35f;
	case BlockType::HEADER3:
		return base * 1.15f;
	case BlockType::SUBTEXT:
		return base * 0.8f;
	default:
		return base;
	}
}

float blockIndent(const Block &b) {
	switch (b.type) {
	case BlockType::QUOTE:
		return 8.0f;
	case BlockType::LIST:
		return (float)b.listDepth * 8.0f;
	case BlockType::CODE_BLOCK:
		return 4.0f;
	default:
		return 0.0f;
	}
}

size_t nextCharBoundary(const std::string &t, size_t i) {
	size_t next = i;
	Utils::Utf8::decodeNext(t, next);
	return (next > i) ? next : i + 1;
}

size_t fitPrefix(const std::string &t, float scale, float maxW) {
	if (measureRichText(t, scale, scale) <= maxW) {
		return t.length();
	}

	size_t fits = 0;
	size_t i = 0;
	while (i < t.length()) {
		size_t next = nextCharBoundary(t, i);
		if (measureRichText(t.substr(0, next), scale, scale) > maxW) {
			break;
		}
		fits = next;
		i = next;
	}

	if (fits == 0) {
		return nextCharBoundary(t, 0);
	}

	size_t space = t.rfind(' ', fits);
	if (space != std::string::npos && space > 0) {
		return space + 1;
	}
	return fits;
}

std::string bulletFor(const Block &b) {
	if (b.type != BlockType::LIST) {
		return "";
	}
	if (b.ordered) {
		return std::to_string(b.listNumber) + ".";
	}
	return "•";
}

Layout buildLayout(const std::string &content, float maxWidth, float baseScale, float ratio, uint16_t allowed,
                   bool allowBlocks) {
	Layout out;
	Options opts;
	opts.allowedStyles = allowed;
	opts.blocks = allowBlocks;
	std::vector<Block> blocks = parse(content, opts);

	for (const Block &block : blocks) {
		float scale = blockScale(block.type, baseScale);
		float indent = blockIndent(block);
		float avail = maxWidth - indent;
		if (avail < 16.0f) {
			avail = 16.0f;
		}

		std::string bullet = bulletFor(block);
		float bulletWidth = bullet.empty() ? 0.0f : measureText(bullet + " ", scale, scale);

		Line line;
		line.type = block.type;
		line.scale = scale;
		line.height = scale * ratio;
		line.indent = indent;
		line.bullet = bullet;
		line.blockStart = true;

		float cursorX = bulletWidth;
		bool anyEmitted = false;

		auto pushLine = [&](bool isEnd) {
			line.blockEnd = isEnd;
			out.lines.push_back(line);
			out.lastLineEndX = indent + cursorX;
			out.lastLineHeight = line.height;
			out.lastLineType = line.type;
			line = Line();
			line.type = block.type;
			line.scale = scale;
			line.height = scale * ratio;
			line.indent = indent;
			line.blockStart = false;
			cursorX = bulletWidth;
			anyEmitted = true;
		};

		for (const Span &span : block.spans) {
			std::string remaining = span.text;

			// A fenced block keeps its own newlines.
			while (true) {
				size_t nl = remaining.find('\n');
				std::string chunk = (nl == std::string::npos) ? remaining : remaining.substr(0, nl);

				while (!chunk.empty()) {
					float room = avail - cursorX;
					size_t take = fitPrefix(chunk, scale, room);

					if (take == 0 ||
					    (measureRichText(chunk.substr(0, take), scale, scale) > room && cursorX > bulletWidth)) {
						pushLine(false);
						continue;
					}

					Piece p;
					p.text = chunk.substr(0, take);
					p.style = span.style;
					p.url = span.url;
					if (span.style & STYLE_SPOILER) {
						out.hasSpoiler = true;
					}
					p.scale = scale;
					p.x = cursorX;
					p.width = measureRichText(p.text, scale, scale);
					line.pieces.push_back(p);
					cursorX += p.width;

					chunk = chunk.substr(take);
					if (!chunk.empty()) {
						pushLine(false);
					}
				}

				if (nl == std::string::npos) {
					break;
				}
				pushLine(false);
				remaining = remaining.substr(nl + 1);
			}
		}

		line.blockEnd = true;
		out.lines.push_back(line);
		out.lastLineEndX = indent + cursorX;
		out.lastLineHeight = line.height;
		out.lastLineType = line.type;
		(void)anyEmitted;
	}

	for (const Line &l : out.lines) {
		out.height += l.height;
	}
	return out;
}

u32 styleColor(uint16_t style, BlockType type, u32 base) {
	if (style & STYLE_LINK) {
		return ScreenManager::colorAccent();
	}
	if (style & STYLE_CODE) {
		return ScreenManager::colorText();
	}
	if (type == BlockType::SUBTEXT || type == BlockType::QUOTE) {
		return ScreenManager::colorTextMuted();
	}
	return base;
}

void drawPiece(const Piece &p, float x, float y, float z, u32 color, BlockType type, float lineH, bool reveal) {
	u32 c = styleColor(p.style, type, color);

	if (p.style & STYLE_CODE) {
		C2D_DrawRectSolid(x - 1.0f, y, z, p.width + 2.0f, lineH - 1.0f, ScreenManager::colorBackgroundDark());
	}

	if (p.style & STYLE_SPOILER) {
		if (!reveal) {
			C2D_DrawRectSolid(x - 1.0f, y, z + 0.01f, p.width + 2.0f, lineH - 1.0f, ScreenManager::colorTextMuted());
			return;
		}
		C2D_DrawRectSolid(x - 1.0f, y, z - 0.01f, p.width + 2.0f, lineH - 1.0f, ScreenManager::colorBackgroundDark());
	}

	if (p.style & STYLE_ITALIC) {
		C3D_Mtx saved;
		C2D_ViewSave(&saved);
		C2D_ViewTranslate(x, y + lineH);
		C2D_ViewShear(-0.18f, 0.0f);
		drawRichText(0.0f, -lineH, z, p.scale, p.scale, c, p.text);
		C2D_ViewRestore(&saved);
	} else {
		drawRichText(x, y, z, p.scale, p.scale, c, p.text);
	}

	if (p.style & STYLE_BOLD) {
		drawRichText(x + 0.5f, y, z, p.scale, p.scale, c, p.text);
	}

	if (p.style & STYLE_UNDERLINE) {
		C2D_DrawRectSolid(x, y + lineH - 3.0f, z, p.width, 1.0f, c);
	}
	if (p.style & STYLE_STRIKE) {
		C2D_DrawRectSolid(x, y + lineH * 0.55f, z, p.width, 1.0f, c);
	}
}

} // namespace

const Layout &get(const std::string &content, float maxWidth, float baseScale, float lineHeightRatio,
                  uint16_t allowedStyles, bool allowBlocks) {
	CacheKey key{content, maxWidth, baseScale, lineHeightRatio, allowedStyles, allowBlocks};

	auto it = cache.find(key);
	if (it != cache.end()) {
		lru.remove(key);
		lru.push_front(key);
		return it->second;
	}

	while (cache.size() >= MAX_CACHE_ENTRIES && !lru.empty()) {
		cache.erase(lru.back());
		lru.pop_back();
	}

	lru.push_front(key);
	return cache.emplace(key, buildLayout(content, maxWidth, baseScale, lineHeightRatio, allowedStyles, allowBlocks))
	    .first->second;
}

float heightOf(const Layout &layout, size_t maxLines) {
	float h = 0.0f;
	size_t n = std::min(maxLines, layout.lines.size());
	for (size_t i = 0; i < n; i++) {
		h += layout.lines[i].height;
	}
	return h;
}

void draw(const Layout &layout, float x, float y, float z, u32 color, size_t maxLines, bool revealSpoilers) {
	float cursorY = y;
	size_t drawn = 0;

	for (const Line &line : layout.lines) {
		if (drawn >= maxLines) {
			break;
		}
		drawn++;
		float lineX = x + line.indent;

		if (line.type == BlockType::QUOTE) {
			C2D_DrawRectSolid(x + 1.0f, cursorY, z, 2.0f, line.height, ScreenManager::colorTextMuted());
		} else if (line.type == BlockType::CODE_BLOCK) {
			C2D_DrawRectSolid(x, cursorY, z - 0.01f, 350.0f, line.height, ScreenManager::colorBackgroundDark());
		}

		if (!line.bullet.empty()) {
			drawText(lineX, cursorY, z, line.scale, line.scale, color, line.bullet);
		}

		for (const Piece &p : line.pieces) {
			drawPiece(p, lineX + p.x, cursorY, z, color, line.type, line.height, revealSpoilers);
		}

		cursorY += line.height;
	}
}

void clearCache() {
	cache.clear();
	lru.clear();
}

} // namespace MarkdownRenderer
} // namespace UI
