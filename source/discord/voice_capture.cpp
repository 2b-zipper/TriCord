#include "discord/voice_capture.h"
#include "log.h"

#include <cstring>
#include <malloc.h>
#include <opus/opus.h>

namespace Discord {

namespace {
constexpr uint32_t MIC_BUFFER_SIZE = 0x10000;
constexpr int OPUS_BITRATE = 24000;
} // namespace

bool VoiceCapture::start() {
	std::lock_guard<std::mutex> lock(mutex);
	if (running) {
		return true;
	}

	micBuffer = (uint8_t *)memalign(0x1000, MIC_BUFFER_SIZE);
	if (!micBuffer) {
		Logger::log("[VoiceCapture] mic buffer allocation failed");
		return false;
	}
	memset(micBuffer, 0, MIC_BUFFER_SIZE);

	if (R_FAILED(micInit(micBuffer, MIC_BUFFER_SIZE))) {
		Logger::log("[VoiceCapture] micInit failed");
		free(micBuffer);
		micBuffer = nullptr;
		return false;
	}

	micBufferSize = micGetSampleDataSize();
	MICU_SetPower(true);
	MICU_SetGain(60);
	MICU_SetClamp(false);

	if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, micBufferSize, true))) {
		Logger::log("[VoiceCapture] StartSampling failed");
		micExit();
		free(micBuffer);
		micBuffer = nullptr;
		return false;
	}

	int err = 0;
	encoder = opus_encoder_create(ENCODE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK || !encoder) {
		Logger::log("[VoiceCapture] opus_encoder_create failed: %d", err);
		MICU_StopSampling();
		micExit();
		free(micBuffer);
		micBuffer = nullptr;
		encoder = nullptr;
		return false;
	}

	opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
	opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
	opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
	opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
	opus_encoder_ctl(encoder, OPUS_SET_DTX(0));

	readOffset = micGetLastSampleOffset();
	resamplePos = 0.0f;
	running = true;
	Logger::log("[VoiceCapture] started (mic buffer %lu bytes)", (unsigned long)micBufferSize);
	return true;
}

void VoiceCapture::stop() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running) {
		return;
	}

	MICU_StopSampling();
	micExit();

	if (encoder) {
		opus_encoder_destroy(encoder);
		encoder = nullptr;
	}
	free(micBuffer);
	micBuffer = nullptr;
	running = false;
	Logger::log("[VoiceCapture] stopped");
}

int VoiceCapture::poll(uint8_t *out, size_t outSize) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running || !encoder) {
		return 0;
	}

	const uint32_t sampleBytes = 2;
	uint32_t writeOffset = micGetLastSampleOffset();

	uint32_t available =
	    (writeOffset >= readOffset) ? (writeOffset - readOffset) : (micBufferSize - readOffset + writeOffset);
	available /= sampleBytes;

	uint32_t needed = (uint32_t)(FRAME_SAMPLES * (MIC_RATE / ENCODE_RATE)) + 2;
	if (available < needed) {
		return 0;
	}

	if (available > (micBufferSize / sampleBytes) / 2) {
		readOffset = writeOffset;
		resamplePos = 0.0f;
		return 0;
	}

	int16_t pcm[FRAME_SAMPLES];
	const float step = MIC_RATE / (float)ENCODE_RATE;
	float pos = resamplePos;
	uint32_t totalSamples = micBufferSize / sampleBytes;

	for (int i = 0; i < FRAME_SAMPLES; i++) {
		uint32_t index = (uint32_t)pos;
		float frac = pos - (float)index;

		uint32_t a = (readOffset / sampleBytes + index) % totalSamples;
		uint32_t b = (a + 1) % totalSamples;

		int16_t sa, sb;
		memcpy(&sa, micBuffer + a * sampleBytes, sampleBytes);
		memcpy(&sb, micBuffer + b * sampleBytes, sampleBytes);

		pcm[i] = (int16_t)((float)sa + ((float)sb - (float)sa) * frac);
		pos += step;
	}

	uint32_t consumed = (uint32_t)pos;
	resamplePos = pos - (float)consumed;
	readOffset = (readOffset + consumed * sampleBytes) % micBufferSize;

	int peak = 0;
	for (int i = 0; i < FRAME_SAMPLES; i++) {
		int magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
		if (magnitude > peak) {
			peak = magnitude;
		}
	}
	peakLevel = peak;

	if (muted) {
		return 0;
	}

	int encoded = opus_encode(encoder, pcm, FRAME_SAMPLES, out, (opus_int32)outSize);
	if (encoded < 0) {
		return 0;
	}
	return encoded;
}

} // namespace Discord
