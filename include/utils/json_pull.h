#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/reader.h>
#include <string>

namespace Utils {
namespace Json {

// MemoryPoolAllocator::Clear() keeps the user buffer, so repeated captures do not allocate.
class ScratchDocument {
  public:
	ScratchDocument()
	    : buffer(new char[BUFFER_SIZE]), allocator(buffer.get(), BUFFER_SIZE, BUFFER_SIZE), doc(&allocator) {}
	rapidjson::Document &get() { return doc; }

  private:
	static constexpr size_t BUFFER_SIZE = 8192;
	std::unique_ptr<char[]> buffer;
	rapidjson::MemoryPoolAllocator<> allocator;
	rapidjson::Document doc;
};

template <typename Stream>
class Pull {
  public:
	enum class Token { NONE, NUL, BOOL, NUMBER, STRING, KEY, START_OBJECT, END_OBJECT, START_ARRAY, END_ARRAY };

	explicit Pull(Stream &stream) : is(stream) { reader.IterativeParseInit(); }

	bool ok() const { return !failed; }
	int errorCode() const { return (int)reader.GetParseErrorCode(); }
	size_t errorOffset() const { return reader.GetErrorOffset(); }

	Token token() const { return rec.token; }
	const std::string &text() const { return rec.text; }
	bool asBool() const { return rec.token == Token::BOOL && rec.boolean; }
	int64_t asInt64() const { return rec.isUnsigned ? (int64_t)rec.u : rec.i; }
	uint64_t asUint64() const { return rec.isUnsigned ? rec.u : (uint64_t)rec.i; }

	bool next() { return step(nullptr); }

	// False at the end of the enclosing object.
	bool nextKey(std::string &key) {
		if (!next() || rec.token == Token::END_OBJECT) {
			return false;
		}
		if (rec.token != Token::KEY) {
			failed = true;
			return false;
		}
		key.swap(rec.text);
		return true;
	}

	// These consume one whole value, and are false at the end of the enclosing container.
	bool skipValue() { return consumeValue(nullptr); }

	bool captureValue(rapidjson::Document &doc) {
		doc.SetNull();
		doc.GetAllocator().Clear();
		bool captured = false;
		auto generate = [&](rapidjson::Document &handler) {
			captured = consumeValue(&handler);
			return captured;
		};
		doc.Populate(generate);
		return captured;
	}

	// Leaves a scalar in token(); a container is skipped and reads as NONE.
	bool readValue() {
		if (!next() || isEnd(rec.token)) {
			return false;
		}
		if (isStart(rec.token)) {
			bool good = skipRest();
			rec.token = Token::NONE;
			return good;
		}
		return true;
	}

	bool readString(std::string &out) {
		if (!readValue()) {
			return false;
		}
		if (rec.token == Token::STRING) {
			out.swap(rec.text);
		} else if (rec.token == Token::NUMBER) {
			out = rec.isUnsigned ? std::to_string(rec.u) : std::to_string(rec.i);
		} else {
			out.clear();
		}
		return true;
	}

	// After next() returned a start token: consumes up to its matching end.
	bool skipRest() { return finishContainer(nullptr); }

  private:
	struct Recorder {
		Token token = Token::NONE;
		std::string text;
		int64_t i = 0;
		uint64_t u = 0;
		bool isUnsigned = false;
		bool boolean = false;
		rapidjson::Document *sink = nullptr;
		// A value's first token may instead close the caller's container.
		bool holdEnd = false;

		bool Null() {
			token = Token::NUL;
			return !sink || sink->Null();
		}
		bool Bool(bool b) {
			token = Token::BOOL;
			boolean = b;
			return !sink || sink->Bool(b);
		}
		bool Int(int v) {
			setSigned(v);
			return !sink || sink->Int(v);
		}
		bool Uint(unsigned v) {
			setUnsigned(v);
			return !sink || sink->Uint(v);
		}
		bool Int64(int64_t v) {
			setSigned(v);
			return !sink || sink->Int64(v);
		}
		bool Uint64(uint64_t v) {
			setUnsigned(v);
			return !sink || sink->Uint64(v);
		}
		bool Double(double d) {
			setSigned((int64_t)d);
			return !sink || sink->Double(d);
		}
		bool RawNumber(const char *s, rapidjson::SizeType length, bool copy) {
			setSigned(strtoll(s, nullptr, 10));
			return !sink || sink->RawNumber(s, length, copy);
		}
		bool String(const char *s, rapidjson::SizeType length, bool copy) {
			token = Token::STRING;
			if (!sink) {
				text.assign(s, length);
				return true;
			}
			return sink->String(s, length, copy);
		}
		bool Key(const char *s, rapidjson::SizeType length, bool copy) {
			token = Token::KEY;
			if (!sink) {
				text.assign(s, length);
				return true;
			}
			return sink->Key(s, length, copy);
		}
		bool StartObject() {
			token = Token::START_OBJECT;
			return !sink || sink->StartObject();
		}
		bool EndObject(rapidjson::SizeType count) {
			token = Token::END_OBJECT;
			return !sink || holdEnd || sink->EndObject(count);
		}
		bool StartArray() {
			token = Token::START_ARRAY;
			return !sink || sink->StartArray();
		}
		bool EndArray(rapidjson::SizeType count) {
			token = Token::END_ARRAY;
			return !sink || holdEnd || sink->EndArray(count);
		}

		void setSigned(int64_t v) {
			token = Token::NUMBER;
			i = v;
			isUnsigned = false;
		}
		void setUnsigned(uint64_t v) {
			token = Token::NUMBER;
			u = v;
			isUnsigned = true;
		}
	};

	static bool isStart(Token t) { return t == Token::START_OBJECT || t == Token::START_ARRAY; }
	static bool isEnd(Token t) { return t == Token::END_OBJECT || t == Token::END_ARRAY; }

	bool step(rapidjson::Document *sink) {
		if (failed || done) {
			failed = true;
			return false;
		}
		rec.token = Token::NONE;
		rec.sink = sink;
		bool good = reader.template IterativeParseNext<rapidjson::kParseDefaultFlags>(is, rec);
		rec.sink = nullptr;
		if (!good || rec.token == Token::NONE) {
			failed = true;
			return false;
		}
		done = reader.IterativeParseComplete();
		return true;
	}

	bool finishContainer(rapidjson::Document *sink) {
		int depth = 1;
		while (depth > 0) {
			if (!step(sink)) {
				return false;
			}
			if (isStart(rec.token)) {
				depth++;
			} else if (isEnd(rec.token)) {
				depth--;
			}
		}
		return true;
	}

	bool consumeValue(rapidjson::Document *sink) {
		rec.holdEnd = true;
		bool good = step(sink);
		rec.holdEnd = false;
		if (!good || isEnd(rec.token)) {
			return false;
		}
		return !isStart(rec.token) || finishContainer(sink);
	}

	Stream &is;
	rapidjson::Reader reader;
	Recorder rec;
	bool failed = false;
	bool done = false;
};

} // namespace Json
} // namespace Utils
