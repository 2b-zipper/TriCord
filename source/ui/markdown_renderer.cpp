#include "ui/markdown_renderer.h"
#include "core/config.h"
#include "ui/screen_manager.h"
#include "utils/utf8_utils.h"

#include <algorithm>
#include <list>
#include <map>
#include <mutex>

namespace UI {
namespace MarkdownRenderer {

using namespace Utils::Markdown;

namespace {

constexpr size_t MAX_CACHE_ENTRIES = 512;

struct CacheKey {
	size_t hash;
	float maxWidth;
	float scale;
	float ratio;
	uint16_t allowed;
	bool blocks;

	bool operator==(const CacheKey &o) const {
		return hash == o.hash && maxWidth == o.maxWidth && scale == o.scale && ratio == o.ratio &&
		       allowed == o.allowed && blocks == o.blocks;
	}

	bool operator<(const CacheKey &o) const {
		if (hash != o.hash) {
			return hash < o.hash;
		}
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
		return blocks < o.blocks;
	}
};

struct CacheEntry {
	Layout layout;
	std::list<CacheKey>::iterator lruIt;
};

std::map<CacheKey, CacheEntry> cache;
std::list<CacheKey> lru;
std::recursive_mutex cacheMutex;

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
					p.color = span.color;
					p.bgColor = span.bgColor;
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

void drawPiece(const Piece &p, float x, float y, float z, u32 color, BlockType type, float lineH, bool reveal, bool isEmbed) {
	u32 c = styleColor(p.style, type, color);
	u32 codeBg = isEmbed ? ScreenManager::colorInput() : ScreenManager::colorBackgroundDark();

	if (p.style & STYLE_MENTION) {
		float mMargin = 2.0f;
		C2D_DrawRectSolid(x - mMargin + 1.0f, y + 1.0f, z - 0.01f, p.width + (mMargin - 1.0f) * 2.0f, lineH - 2.0f, p.bgColor);
		C2D_DrawRectSolid(x - mMargin, y + 2.0f, z - 0.01f, p.width + mMargin * 2.0f, lineH - 4.0f, p.bgColor);
		c = p.color;
	}

	if (p.style & STYLE_CODE) {
		C2D_DrawRectSolid(x - 1.0f, y, z, p.width + 2.0f, lineH - 1.0f, codeBg);
	}

	if (p.style & STYLE_SPOILER) {
		if (!reveal) {
			C2D_DrawRectSolid(x - 1.0f, y, z + 0.01f, p.width + 2.0f, lineH - 1.0f, ScreenManager::colorTextMuted());
			return;
		}
		C2D_DrawRectSolid(x - 1.0f, y, z - 0.01f, p.width + 2.0f, lineH - 1.0f, codeBg);
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

Layout get(const std::string &content, float maxWidth, float scale, float ratio, uint16_t allowed,
           bool allowBlocks) {
	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	size_t hash = std::hash<std::string>{}(content);
	CacheKey k = {hash, maxWidth, scale, ratio, allowed, allowBlocks};
	auto it = cache.find(k);
	if (it != cache.end()) {
		lru.erase(it->second.lruIt);
		lru.push_front(k);
		it->second.lruIt = lru.begin();
		return it->second.layout;
	}

	Layout l = buildLayout(content, maxWidth, scale, ratio, allowed, allowBlocks);
	if (cache.size() >= MAX_CACHE_ENTRIES) {
		cache.erase(lru.back());
		lru.pop_back();
	}
	lru.push_front(k);
	CacheEntry entry = {l, lru.begin()};
	auto res = cache.insert({k, entry});
	return res.first->second.layout;
}

float heightOf(const Layout &layout, size_t maxLines) {
	float h = 0.0f;
	size_t n = std::min(maxLines, layout.lines.size());
	for (size_t i = 0; i < n; i++) {
		h += layout.lines[i].height;
	}
	return h;
}

void draw(const Layout &layout, float x, float y, float z, u32 color, size_t maxLines, bool revealSpoilers, bool isEmbed) {
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
			u32 codeBg = isEmbed ? ScreenManager::colorInput() : ScreenManager::colorBackgroundDark();
			C2D_DrawRectSolid(x, cursorY, z - 0.01f, 350.0f, line.height, codeBg);
		}

		if (!line.bullet.empty()) {
			drawText(lineX, cursorY, z, line.scale, line.scale, color, line.bullet);
		}

		for (const Piece &p : line.pieces) {
			drawPiece(p, lineX + p.x, cursorY, z, color, line.type, line.height, revealSpoilers, isEmbed);
		}

		cursorY += line.height;
	}
}

void clearCache() {
	std::lock_guard<std::recursive_mutex> lock(cacheMutex);
	cache.clear();
	lru.clear();
}

} // namespace MarkdownRenderer
} // namespace UI
