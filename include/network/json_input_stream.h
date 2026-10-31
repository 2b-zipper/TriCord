#ifndef JSON_INPUT_STREAM_H
#define JSON_INPUT_STREAM_H

#include <cstddef>
#include <memory>
#include <rapidjson/rapidjson.h>
#include <string>
#include <vector>

struct z_stream_s;

namespace Network {

// Discord's zlib-stream transport shares one inflate context across messages.
class ZlibInflater {
  public:
	ZlibInflater();
	~ZlibInflater();
	ZlibInflater(const ZlibInflater &) = delete;
	ZlibInflater &operator=(const ZlibInflater &) = delete;

	bool reset();
	// data must outlive every read() of this message.
	void setInput(const std::string &data);
	size_t read(char *out, size_t capacity);

	char *buffer() { return chunk.data(); }
	size_t bufferSize() const { return chunk.size(); }

  private:
	std::unique_ptr<z_stream_s> stream;
	std::vector<char> chunk;
	bool error = false;
};

// A rapidjson stream that inflates on demand, so a message is never decompressed as a whole.
class JsonInputStream {
  public:
	typedef char Ch;

	explicit JsonInputStream(ZlibInflater &inflater);
	JsonInputStream(const char *text, size_t size);

	Ch Peek() const { return pos < len ? data[pos] : '\0'; }
	Ch Take() {
		if (pos >= len) {
			return '\0';
		}
		Ch c = data[pos++];
		if (pos == len) {
			refill();
		}
		return c;
	}
	size_t Tell() const { return consumed + pos; }

	Ch *PutBegin() {
		RAPIDJSON_ASSERT(false);
		return nullptr;
	}
	void Put(Ch) { RAPIDJSON_ASSERT(false); }
	size_t PutEnd(Ch *) {
		RAPIDJSON_ASSERT(false);
		return 0;
	}

	// Before the first Take(), what is read so far is the start of the message.
	const char *pending() const { return data + pos; }
	size_t pendingSize() const { return len - pos; }

	std::string readAll();
	void drain();

  private:
	void refill();

	ZlibInflater *inflater = nullptr;
	const char *data = nullptr;
	size_t pos = 0;
	size_t len = 0;
	size_t consumed = 0;
};

} // namespace Network

#endif // JSON_INPUT_STREAM_H
