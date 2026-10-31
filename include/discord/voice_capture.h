#ifndef VOICE_CAPTURE_H
#define VOICE_CAPTURE_H

#include "discord/echo_canceller.h"

#include <3ds.h>
#include <atomic>
#include <cstdint>
#include <mutex>

struct OpusEncoder;

namespace Discord {

class VoiceCapture {
  public:
	static constexpr int ENCODE_RATE = 16000;
	static constexpr int FRAME_SAMPLES = ENCODE_RATE / 50;
	// MICU_SAMPLE_RATE_16360 is actually 16364.479Hz.
	static constexpr double MIC_RATE = 16364.479;

	bool start();
	void stop();
	bool isRunning() const { return running; }

	void setMuted(bool m) { muted = m; }
	bool isMuted() const { return muted; }

	void setEchoCanceller(EchoCanceller *e) { echo = e; }

	int lastPeak() const { return peakLevel; }

	bool readFrame(int16_t *pcm);

	int backlogFrames() const { return backlog; }

	int encode(const int16_t *pcm, uint8_t *out, size_t outSize);

  private:
	static constexpr uint32_t STEP_Q20 = (uint32_t)(MIC_RATE * 1048576.0 / ENCODE_RATE + 0.5);
	static constexpr uint32_t SRC_PER_FRAME = ((uint32_t)FRAME_SAMPLES * STEP_Q20 >> 20) + 2;

	bool running = false;
	std::atomic<bool> muted{false};
	std::atomic<int> peakLevel{0};
	EchoCanceller *echo = nullptr;

	uint8_t *micBuffer = nullptr;
	uint32_t micBufferSize = 0;
	uint32_t readOffset = 0;
	uint32_t resamplePos = 0;
	int backlog = 0;

	OpusEncoder *encoder = nullptr;
	std::mutex mutex;
};

} // namespace Discord

#endif // VOICE_CAPTURE_H
