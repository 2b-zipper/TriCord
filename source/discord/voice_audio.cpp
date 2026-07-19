#include "discord/voice_audio.h"
#include "log.h"

#include <cstring>
#include <malloc.h>
#include <opus/opus.h>

namespace Discord {

namespace {
constexpr int NDSP_CHANNEL = 0;
} // namespace

bool VoiceAudio::start() {
	std::lock_guard<std::mutex> lock(mutex);
	if (running) {
		return true;
	}

	ring = (int16_t *)calloc((size_t)RING_FRAMES * FRAME_SAMPLES, sizeof(int16_t));
	waveData = (int16_t *)linearAlloc(sizeof(waveBufs) / sizeof(waveBufs[0]) * FRAME_SAMPLES * sizeof(int16_t));
	if (!ring || !waveData) {
		Logger::log("[VoiceAudio] allocation failed");
		free(ring);
		ring = nullptr;
		if (waveData) {
			linearFree(waveData);
			waveData = nullptr;
		}
		return false;
	}

	ndspChnReset(NDSP_CHANNEL);
	ndspChnSetInterp(NDSP_CHANNEL, NDSP_INTERP_NONE);
	ndspChnSetRate(NDSP_CHANNEL, (float)SAMPLE_RATE);
	ndspChnSetFormat(NDSP_CHANNEL, NDSP_FORMAT_MONO_PCM16);

	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0f;
	mix[1] = 1.0f;
	ndspChnSetMix(NDSP_CHANNEL, mix);

	memset(waveBufs, 0, sizeof(waveBufs));
	for (int i = 0; i < 4; i++) {
		waveBufs[i].data_vaddr = waveData + (size_t)i * FRAME_SAMPLES;
		waveBufs[i].nsamples = FRAME_SAMPLES;
		waveBufs[i].status = NDSP_WBUF_DONE;
	}

	playFrame = 0;
	writtenFrames = 0;
	nextWaveBuf = 0;
	running = true;
	Logger::log("[VoiceAudio] started");
	return true;
}

void VoiceAudio::stop() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running) {
		return;
	}

	ndspChnWaveBufClear(NDSP_CHANNEL);

	for (auto &pair : speakers) {
		if (pair.second.decoder) {
			opus_decoder_destroy(pair.second.decoder);
		}
	}
	speakers.clear();

	free(ring);
	ring = nullptr;
	linearFree(waveData);
	waveData = nullptr;
	running = false;
	Logger::log("[VoiceAudio] stopped");
}

void VoiceAudio::mixFrame(size_t frameIndex, const int16_t *samples) {
	int16_t *dst = ring + (frameIndex % RING_FRAMES) * FRAME_SAMPLES;
	for (int i = 0; i < FRAME_SAMPLES; i++) {
		int32_t sum = (int32_t)dst[i] + samples[i];
		if (sum > 32767) {
			sum = 32767;
		} else if (sum < -32768) {
			sum = -32768;
		}
		dst[i] = (int16_t)sum;
	}
}

void VoiceAudio::pushOpus(uint32_t ssrc, const uint8_t *data, size_t len) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running || deafened) {
		return;
	}

	Speaker &speaker = speakers[ssrc];
	if (!speaker.decoder) {
		int err = 0;
		speaker.decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
		if (err != OPUS_OK || !speaker.decoder) {
			Logger::log("[VoiceAudio] opus_decoder_create failed for ssrc %lu: %d", (unsigned long)ssrc, err);
			speaker.decoder = nullptr;
			return;
		}
		speaker.primed = false;
		Logger::log("[VoiceAudio] new speaker ssrc=%lu", (unsigned long)ssrc);
	}

	int16_t pcm[FRAME_SAMPLES];
	int decoded = opus_decode(speaker.decoder, data, (opus_int32)len, pcm, FRAME_SAMPLES, 0);
	if (decoded <= 0) {
		return;
	}
	if (decoded < FRAME_SAMPLES) {
		memset(pcm + decoded, 0, (FRAME_SAMPLES - decoded) * sizeof(int16_t));
	}

	if (!speaker.primed) {
		speaker.writeFrame = playFrame + PREBUFFER_FRAMES;
		speaker.primed = true;
	}

	if (speaker.writeFrame < playFrame || speaker.writeFrame >= playFrame + RING_FRAMES) {
		speaker.writeFrame = playFrame + PREBUFFER_FRAMES;
	}

	mixFrame(speaker.writeFrame, pcm);
	if (speaker.writeFrame + 1 > writtenFrames) {
		writtenFrames = speaker.writeFrame + 1;
	}
	speaker.writeFrame++;
}

void VoiceAudio::setDeafened(bool d) {
	std::lock_guard<std::mutex> lock(mutex);
	if (deafened == d) {
		return;
	}
	deafened = d;
	if (!d) {
		return;
	}

	if (ring) {
		memset(ring, 0, RING_FRAMES * FRAME_SAMPLES * sizeof(int16_t));
	}
	writtenFrames = playFrame;
	for (auto &pair : speakers) {
		pair.second.primed = false;
	}
}

void VoiceAudio::dropSpeaker(uint32_t ssrc) {
	std::lock_guard<std::mutex> lock(mutex);
	auto it = speakers.find(ssrc);
	if (it == speakers.end()) {
		return;
	}
	if (it->second.decoder) {
		opus_decoder_destroy(it->second.decoder);
	}
	speakers.erase(it);
}

void VoiceAudio::pump() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running) {
		return;
	}

	for (int i = 0; i < 4; i++) {
		ndspWaveBuf &buf = waveBufs[nextWaveBuf];
		if (buf.status != NDSP_WBUF_FREE && buf.status != NDSP_WBUF_DONE) {
			break;
		}
		if (playFrame >= writtenFrames) {
			break;
		}

		int16_t *src = ring + (playFrame % RING_FRAMES) * FRAME_SAMPLES;
		int16_t *dst = (int16_t *)buf.data_vaddr;
		memcpy(dst, src, FRAME_SAMPLES * sizeof(int16_t));
		memset(src, 0, FRAME_SAMPLES * sizeof(int16_t));

		DSP_FlushDataCache(dst, FRAME_SAMPLES * sizeof(int16_t));
		ndspChnWaveBufAdd(NDSP_CHANNEL, &buf);

		playFrame++;
		nextWaveBuf = (nextWaveBuf + 1) % 4;
	}
}

} // namespace Discord
