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
	// Constrained, so a loud frame cannot spike far above the average and arrive
	// late behind the others.
	opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(1));
	opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
	opus_encoder_ctl(encoder, OPUS_SET_DTX(0));

	readOffset = micGetLastSampleOffset();
	resamplePos = 0;
	backlog = 0;
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

bool VoiceCapture::readFrame(int16_t *pcm) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running || !micBuffer) {
		return false;
	}

	const uint32_t totalSamples = micBufferSize / 2;
	const uint32_t writeOffset = micGetLastSampleOffset();

	uint32_t available =
	    (writeOffset >= readOffset) ? (writeOffset - readOffset) : (micBufferSize - readOffset + writeOffset);
	available /= 2;

	if (available < SRC_PER_FRAME) {
		backlog = 0;
		return false;
	}

	// A second behind or more: the stream is not worth catching up on.
	if (available > totalSamples / 2) {
		readOffset = writeOffset;
		resamplePos = 0;
		backlog = 0;
		return false;
	}

	const int16_t *samples = (const int16_t *)micBuffer;
	const uint32_t base = readOffset / 2;
	uint32_t pos = resamplePos;

	for (int i = 0; i < FRAME_SAMPLES; i++) {
		uint32_t a = base + (pos >> 20);
		if (a >= totalSamples) {
			a -= totalSamples;
		}
		uint32_t b = a + 1;
		if (b >= totalSamples) {
			b -= totalSamples;
		}

		const int32_t sa = samples[a];
		const int32_t sb = samples[b];
		const int32_t frac = (int32_t)((pos >> 6) & 0x3FFF); // 14 bits
		pcm[i] = (int16_t)(sa + (((sb - sa) * frac) >> 14));
		pos += STEP_Q20;
	}

	const uint32_t consumed = pos >> 20;
	resamplePos = pos & 0xFFFFF;
	uint32_t next = base + consumed;
	if (next >= totalSamples) {
		next -= totalSamples;
	}
	readOffset = next * 2;

	backlog = (int)((available - consumed) / SRC_PER_FRAME);

	// Ahead of the peak, so the speaking indicator ignores the speaker's output.
	if (echo) {
		echo->process(pcm, FRAME_SAMPLES);
	}

	int peak = 0;
	for (int i = 0; i < FRAME_SAMPLES; i++) {
		const int magnitude = pcm[i] < 0 ? -pcm[i] : pcm[i];
		if (magnitude > peak) {
			peak = magnitude;
		}
	}
	peakLevel = peak;
	return true;
}

int VoiceCapture::encode(const int16_t *pcm, uint8_t *out, size_t outSize) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running || !encoder) {
		return 0;
	}
	const int encoded = opus_encode(encoder, pcm, FRAME_SAMPLES, out, (opus_int32)outSize);
	return encoded < 0 ? 0 : encoded;
}

} // namespace Discord
