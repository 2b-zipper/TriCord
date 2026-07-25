#include "utils/sound_player.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <malloc.h>

namespace Utils {

namespace {
constexpr int NDSP_CHANNEL = 1;
constexpr int WAVE_BUF_COUNT = 4;

const char *const SOUND_PATHS[(size_t)Sound::COUNT] = {
    "romfs:/discord-sounds/join.pcm",          "romfs:/discord-sounds/left.pcm",
    "romfs:/discord-sounds/left2.pcm",         "romfs:/discord-sounds/mic_on.pcm",
    "romfs:/discord-sounds/mic_off.pcm",       "romfs:/discord-sounds/headphone_on.pcm",
    "romfs:/discord-sounds/headphone_off.pcm", "romfs:/discord-sounds/call_outgoing.pcm",
    "romfs:/discord-sounds/call_incoming.pcm", "romfs:/discord-sounds/notification.pcm",
};
} // namespace

SoundPlayer &SoundPlayer::getInstance() {
	static SoundPlayer instance;
	return instance;
}

bool SoundPlayer::load(Sound sound, const std::string &path) {
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		Logger::log("[Sound] missing %s", path.c_str());
		return false;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || (size % 2) != 0) {
		fclose(f);
		Logger::log("[Sound] bad size for %s", path.c_str());
		return false;
	}

	int16_t *samples = (int16_t *)linearAlloc((size_t)size);
	if (!samples) {
		fclose(f);
		Logger::log("[Sound] alloc failed for %s", path.c_str());
		return false;
	}

	size_t read = fread(samples, 1, (size_t)size, f);
	fclose(f);

	if (read != (size_t)size) {
		linearFree(samples);
		Logger::log("[Sound] short read for %s", path.c_str());
		return false;
	}

	DSP_FlushDataCache(samples, (size_t)size);

	Clip &clip = clips[(size_t)sound];
	clip.samples = samples;
	clip.count = (size_t)size / sizeof(int16_t);
	return true;
}

void SoundPlayer::init() {
	std::lock_guard<std::mutex> lock(mutex);
	if (ready) {
		return;
	}

	ndspChnReset(NDSP_CHANNEL);
	ndspChnSetInterp(NDSP_CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(NDSP_CHANNEL, (float)SAMPLE_RATE);
	ndspChnSetFormat(NDSP_CHANNEL, NDSP_FORMAT_MONO_PCM16);

	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0f;
	mix[1] = 1.0f;
	ndspChnSetMix(NDSP_CHANNEL, mix);

	memset(waveBufs, 0, sizeof(waveBufs));
	for (int i = 0; i < WAVE_BUF_COUNT; i++) {
		waveBufs[i].status = NDSP_WBUF_DONE;
	}

	int loaded = 0;
	for (size_t i = 0; i < (size_t)Sound::COUNT; i++) {
		if (load((Sound)i, SOUND_PATHS[i])) {
			loaded++;
		}
	}

	ready = true;
	Logger::log("[Sound] %d/%d clips loaded", loaded, (int)Sound::COUNT);
}

void SoundPlayer::shutdown() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!ready) {
		return;
	}

	ndspChnWaveBufClear(NDSP_CHANNEL);
	for (auto &clip : clips) {
		if (clip.samples) {
			linearFree(clip.samples);
			clip.samples = nullptr;
			clip.count = 0;
		}
	}
	ready = false;
}

void SoundPlayer::stop() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!ready) {
		return;
	}
	ndspChnWaveBufClear(NDSP_CHANNEL);
	for (int i = 0; i < WAVE_BUF_COUNT; i++) {
		waveBufs[i].status = NDSP_WBUF_DONE;
	}
	nextWaveBuf = 0;
}

int SoundPlayer::clipFrames(Sound sound) {
	std::lock_guard<std::mutex> lock(mutex);
	const Clip &clip = clips[(size_t)sound];
	if (!clip.samples || clip.count == 0) {
		return 0;
	}
	return (int)((clip.count * 60 + SAMPLE_RATE - 1) / SAMPLE_RATE);
}

void SoundPlayer::play(Sound sound) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!ready) {
		return;
	}

	const Clip &clip = clips[(size_t)sound];
	if (!clip.samples) {
		return;
	}

	int slot = -1;
	for (int i = 0; i < WAVE_BUF_COUNT; i++) {
		int candidate = (nextWaveBuf + i) % WAVE_BUF_COUNT;
		if (waveBufs[candidate].status == NDSP_WBUF_DONE || waveBufs[candidate].status == NDSP_WBUF_FREE) {
			slot = candidate;
			break;
		}
	}

	if (slot < 0) {
		ndspChnWaveBufClear(NDSP_CHANNEL);
		slot = 0;
	}

	ndspWaveBuf &buf = waveBufs[slot];
	memset(&buf, 0, sizeof(buf));
	buf.data_vaddr = clip.samples;
	buf.nsamples = (u32)clip.count;
	ndspChnWaveBufAdd(NDSP_CHANNEL, &buf);

	nextWaveBuf = (slot + 1) % WAVE_BUF_COUNT;
}

} // namespace Utils
