#include "network/json_input_stream.h"
#include "log.h"

#include <cstring>
#include <zlib.h>

namespace Network {

namespace {
constexpr size_t CHUNK_SIZE = 16 * 1024;
}

ZlibInflater::ZlibInflater() : chunk(CHUNK_SIZE) {}

ZlibInflater::~ZlibInflater() {
	if (stream) {
		inflateEnd(stream.get());
	}
}

bool ZlibInflater::reset() {
	error = false;
	if (stream) {
		error = inflateReset(stream.get()) != Z_OK;
		return !error;
	}

	std::unique_ptr<z_stream_s> zs(new z_stream_s);
	memset(zs.get(), 0, sizeof(*zs));
	if (inflateInit(zs.get()) != Z_OK) {
		error = true;
		return false;
	}
	stream = std::move(zs);
	return true;
}

void ZlibInflater::setInput(const std::string &data) {
	if (!stream) {
		return;
	}
	stream->next_in = (Bytef *)data.data();
	stream->avail_in = (uInt)data.size();
}

size_t ZlibInflater::read(char *out, size_t capacity) {
	if (!stream || error) {
		return 0;
	}

	stream->next_out = (Bytef *)out;
	stream->avail_out = (uInt)capacity;
	int ret = inflate(stream.get(), Z_SYNC_FLUSH);
	if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
		Logger::log("WS inflate failed: %d", ret);
		error = true;
		stream->avail_in = 0;
		return 0;
	}
	return capacity - stream->avail_out;
}

JsonInputStream::JsonInputStream(ZlibInflater &source) : inflater(&source), data(source.buffer()) { refill(); }

JsonInputStream::JsonInputStream(const char *text, size_t size) : data(text), len(size) {}

void JsonInputStream::refill() {
	consumed += len;
	pos = 0;
	len = inflater ? inflater->read(inflater->buffer(), inflater->bufferSize()) : 0;
}

std::string JsonInputStream::readAll() {
	std::string out;
	while (len > 0) {
		out.append(data + pos, len - pos);
		refill();
	}
	return out;
}

void JsonInputStream::drain() {
	while (len > 0) {
		refill();
	}
}

} // namespace Network
