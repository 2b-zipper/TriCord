#include "utils/markdown.h"

#include <cstdlib>
#include <cstring>

namespace Utils {
namespace Markdown {

namespace {

bool isWordChar(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80;
}

size_t atomEnd(const std::string &t, size_t i) {
	if (i >= t.length() || t[i] != '<') {
		return 0;
	}
	size_t p = i + 1;
	if (p >= t.length()) {
		return 0;
	}
	bool known =
	    (t[p] == ':' || t[p] == '@' || t[p] == '#' || t.compare(p, 2, "a:") == 0 || t.compare(p, 2, "t:") == 0);
	if (!known) {
		return 0;
	}
	size_t close = t.find('>', p);
	if (close == std::string::npos || close - i > 96) {
		return 0;
	}
	return close + 1;
}

void flush(std::string &buf, uint16_t style, const std::string &url, std::vector<Span> &out) {
	if (buf.empty()) {
		return;
	}
	Span s;
	s.text = buf;
	s.style = style;
	s.url = url;
	out.push_back(s);
	buf.clear();
}

size_t findClose(const std::string &t, size_t from, const char *delim) {
	size_t dl = strlen(delim);
	size_t i = from;
	while (i < t.length()) {
		if (t[i] == '\\') {
			i += 2;
			continue;
		}
		size_t ae = atomEnd(t, i);
		if (ae) {
			i = ae;
			continue;
		}
		if (t.compare(i, dl, delim) == 0) {
			return i;
		}
		i++;
	}
	return std::string::npos;
}

void parseInline(const std::string &t, uint16_t style, const std::string &url, std::vector<Span> &out,
                 uint16_t allowed);

bool tryEmphasis(const std::string &t, size_t &i, uint16_t style, const std::string &url, std::string &buf,
                 std::vector<Span> &out, uint16_t allowed) {
	struct Rule {
		const char *delim;
		uint16_t flags;
	};
	static const Rule rules[] = {
	    {"***", STYLE_BOLD | STYLE_ITALIC},
	    {"**", STYLE_BOLD},
	    {"___", STYLE_UNDERLINE | STYLE_ITALIC},
	    {"__", STYLE_UNDERLINE},
	    {"~~", STYLE_STRIKE},
	    {"||", STYLE_SPOILER},
	    {"*", STYLE_ITALIC},
	    {"_", STYLE_ITALIC},
	};

	for (const Rule &r : rules) {
		size_t dl = strlen(r.delim);
		if (t.compare(i, dl, r.delim) != 0) {
			continue;
		}
		if ((r.flags & allowed) != r.flags) {
			continue;
		}
		if (style & r.flags & (STYLE_BOLD | STYLE_ITALIC | STYLE_UNDERLINE | STYLE_STRIKE | STYLE_SPOILER)) {
			continue;
		}

		if (r.delim[0] == '_') {
			if (i > 0 && isWordChar(t[i - 1])) {
				continue;
			}
		}

		size_t close = findClose(t, i + dl, r.delim);
		if (close == std::string::npos || close == i + dl) {
			continue;
		}
		if (r.delim[0] == '_' && close + dl < t.length() && isWordChar(t[close + dl])) {
			continue;
		}

		flush(buf, style, url, out);
		parseInline(t.substr(i + dl, close - i - dl), style | r.flags, url, out, allowed);
		i = close + dl;
		return true;
	}
	return false;
}

void parseInline(const std::string &t, uint16_t style, const std::string &url, std::vector<Span> &out,
                 uint16_t allowed) {
	std::string buf;
	size_t i = 0;

	while (i < t.length()) {
		if (t[i] == '\\' && i + 1 < t.length()) {
			buf += t[i + 1];
			i += 2;
			continue;
		}

		size_t ae = atomEnd(t, i);
		if (ae) {
			buf.append(t, i, ae - i);
			i = ae;
			continue;
		}

		if (t[i] == '`' && (allowed & STYLE_CODE)) {
			const char *delim = (t.compare(i, 2, "``") == 0) ? "``" : "`";
			size_t dl = strlen(delim);
			size_t close = t.find(delim, i + dl);
			if (close != std::string::npos && close > i + dl) {
				flush(buf, style, url, out);
				Span s;
				s.text = t.substr(i + dl, close - i - dl);
				s.style = style | STYLE_CODE;
				s.url = url;
				out.push_back(s);
				i = close + dl;
				continue;
			}
		}

		if (tryEmphasis(t, i, style, url, buf, out, allowed)) {
			continue;
		}

		if (t[i] == '[' && (allowed & STYLE_LINK)) {
			size_t rb = findClose(t, i + 1, "]");
			if (rb != std::string::npos && rb + 1 < t.length() && t[rb + 1] == '(') {
				size_t rp = t.find(')', rb + 2);
				if (rp != std::string::npos) {
					flush(buf, style, url, out);
					parseInline(t.substr(i + 1, rb - i - 1), style | STYLE_LINK, t.substr(rb + 2, rp - rb - 2), out,
					            allowed);
					i = rp + 1;
					continue;
				}
			}
		}

		if (t[i] == '\x01') {
			size_t close = t.find('\x02', i + 1);
			if (close != std::string::npos) {
				size_t sc1 = t.find(';', i + 1);
				size_t sc2 = t.find(';', sc1 + 1);
				if (sc1 != std::string::npos && sc2 != std::string::npos && sc2 < close) {
					uint32_t fg = std::stoul(t.substr(i + 1, sc1 - i - 1));
					uint32_t bg = std::stoul(t.substr(sc1 + 1, sc2 - sc1 - 1));
					std::string mText = t.substr(sc2 + 1, close - sc2 - 1);
					
					flush(buf, style, url, out);
					Span s;
					s.text = mText;
					s.style = style | STYLE_MENTION;
					s.color = fg;
					s.bgColor = bg;
					s.url = url;
					out.push_back(s);
					
					i = close + 1;
					continue;
				}
			}
		}

		buf += t[i];
		i++;
	}

	flush(buf, style, url, out);
}

std::vector<std::string> splitLines(const std::string &text) {
	std::vector<std::string> lines;
	size_t start = 0;
	while (true) {
		size_t nl = text.find('\n', start);
		if (nl == std::string::npos) {
			lines.push_back(text.substr(start));
			break;
		}
		std::string line = text.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		lines.push_back(line);
		start = nl + 1;
	}
	return lines;
}

size_t countIndent(const std::string &line) {
	size_t n = 0;
	while (n < line.length() && line[n] == ' ') {
		n++;
	}
	return n;
}

std::string trimRight(const std::string &s) {
	size_t e = s.length();
	while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
		e--;
	}
	return s.substr(0, e);
}

bool parseOrderedMarker(const std::string &line, size_t indent, size_t &contentStart, int &number) {
	size_t p = indent;
	size_t digitsStart = p;
	while (p < line.length() && line[p] >= '0' && line[p] <= '9') {
		p++;
	}
	if (p == digitsStart || p >= line.length() || line[p] != '.') {
		return false;
	}
	if (p + 1 >= line.length() || line[p + 1] != ' ') {
		return false;
	}
	number = atoi(line.substr(digitsStart, p - digitsStart).c_str());
	contentStart = p + 2;
	return true;
}

} // namespace

std::vector<Block> parse(const std::string &text, const Options &options) {
	std::vector<Block> blocks;
	std::vector<std::string> lines = splitLines(text);
	bool inTripleQuote = false;

	if (!options.blocks) {
		for (const std::string &line : lines) {
			Block b;
			b.type = BlockType::PARAGRAPH;
			parseInline(line, STYLE_NONE, "", b.spans, options.allowedStyles);
			blocks.push_back(b);
		}
		return blocks;
	}

	for (size_t li = 0; li < lines.size(); li++) {
		const std::string &line = lines[li];

		if (line.compare(0, 3, "```") == 0) {
			std::string rest = line.substr(3);
			Block b;
			b.type = BlockType::CODE_BLOCK;

			size_t sameLineEnd = rest.find("```");
			if (sameLineEnd != std::string::npos) {
				Span s;
				s.text = rest.substr(0, sameLineEnd);
				s.style = STYLE_CODE;
				b.spans.push_back(s);
				blocks.push_back(b);
				continue;
			}

			if (rest.find(' ') == std::string::npos) {
				b.language = trimRight(rest);
			}

			std::string body;
			size_t j = li + 1;
			bool closed = false;
			for (; j < lines.size(); j++) {
				if (trimRight(lines[j]) == "```") {
					closed = true;
					break;
				}
				if (!body.empty()) {
					body += "\n";
				}
				body += lines[j];
			}

			if (!closed) {
				b.language.clear();
				body = rest;
				for (size_t k = li + 1; k < lines.size(); k++) {
					body += "\n" + lines[k];
				}
			}

			Span s;
			s.text = body;
			s.style = STYLE_CODE;
			b.spans.push_back(s);
			blocks.push_back(b);
			li = closed ? j : lines.size();
			continue;
		}

		size_t indent = countIndent(line);
		std::string body = line.substr(indent);

		if (inTripleQuote) {
			Block b;
			b.type = BlockType::QUOTE;
			parseInline(line, STYLE_NONE, "", b.spans, options.allowedStyles);
			blocks.push_back(b);
			continue;
		}

		Block b;
		std::string content;

		if (body.compare(0, 4, ">>> ") == 0) {
			inTripleQuote = true;
			b.type = BlockType::QUOTE;
			content = body.substr(4);
		} else if (body.compare(0, 2, "> ") == 0) {
			b.type = BlockType::QUOTE;
			content = body.substr(2);
		} else if (body.compare(0, 3, "-# ") == 0) {
			b.type = BlockType::SUBTEXT;
			content = body.substr(3);
		} else if (body.compare(0, 4, "### ") == 0) {
			b.type = BlockType::HEADER3;
			content = body.substr(4);
		} else if (body.compare(0, 3, "## ") == 0) {
			b.type = BlockType::HEADER2;
			content = body.substr(3);
		} else if (body.compare(0, 2, "# ") == 0) {
			b.type = BlockType::HEADER1;
			content = body.substr(2);
		} else if (body.compare(0, 2, "- ") == 0 || body.compare(0, 2, "* ") == 0) {
			b.type = BlockType::LIST;
			b.listDepth = (int)(indent / 2);
			content = body.substr(2);
		} else {
			size_t contentStart = 0;
			int number = 0;
			if (parseOrderedMarker(line, indent, contentStart, number)) {
				b.type = BlockType::LIST;
				b.ordered = true;
				b.listNumber = number;
				b.listDepth = (int)(indent / 2);
				content = line.substr(contentStart);
			} else {
				b.type = BlockType::PARAGRAPH;
				content = line;
			}
		}

		parseInline(content, STYLE_NONE, "", b.spans, options.allowedStyles);
		blocks.push_back(b);
	}

	return blocks;
}

std::string stripFormatting(const std::string &text) {
	std::vector<Block> blocks = parse(text);
	std::string out;
	for (size_t i = 0; i < blocks.size(); i++) {
		if (i > 0) {
			out += "\n";
		}
		for (const Span &s : blocks[i].spans) {
			out += s.text;
		}
	}
	return out;
}

} // namespace Markdown
} // namespace Utils
