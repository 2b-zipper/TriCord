#ifndef VOICE_CAPTURE_H
#define VOICE_CAPTURE_H

#include "discord/echo_canceller.h"

#include <3ds.h>
#include <cstdint>
#include <atomic>
#include <mutex>

struct OpusEncoder;

namespace Discord {

class VoiceCapture {
  public:
	static constexpr int ENCODE_RATE = 16000;
	static constexpr int FRAME_SAMPLES = ENCODE_RATE / 50;
	// MICU_SAMPLE_RATE_16360 is actually 16364.479Hz.
	static constexpr float MIC_RATE = 16364.479f;

	bool start();
	void stop();
	bool isRunning() const { return running; }

	void setMuted(bool m) { muted = m; }
	bool isMuted() const { return muted; }

	void setEchoCanceller(EchoCanceller *e) { echo = e; }

	int lastPeak() const { return peakLevel; }

	int poll(uint8_t *out, size_t outSize);

  private:
	bool running = false;
	std::atomic<bool> muted{false};
	std::atomic<int> peakLevel{0};
	EchoCanceller *echo = nullptr;

	uint8_t *micBuffer = nullptr;
	uint32_t micBufferSize = 0;
	uint32_t readOffset = 0;

	OpusEncoder *encoder = nullptr;
	float resamplePos = 0.0f;
	std::mutex mutex;
};

} // namespace Discord

#endif // VOICE_CAPTURE_H
