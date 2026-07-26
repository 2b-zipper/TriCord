#ifndef VOICE_AUDIO_H
#define VOICE_AUDIO_H

#include "discord/echo_canceller.h"

#include <3ds.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

struct OpusDecoder;

namespace Discord {

class VoiceAudio {
  public:
	static constexpr int SAMPLE_RATE = 48000;
	static constexpr int FRAME_SAMPLES = 960;
	static constexpr int RING_FRAMES = 24;
	static constexpr int PREBUFFER_FRAMES = 3;

	bool start();
	void stop();
	bool isRunning() const { return running; }

	void pushOpus(uint32_t ssrc, const uint8_t *data, size_t len);
	void dropSpeaker(uint32_t ssrc);

	void setDeafened(bool d);
	bool isDeafened() const { return deafened; }

	void setEchoCanceller(EchoCanceller *e) { echo = e; }

	void pump();

  private:
	struct Speaker {
		OpusDecoder *decoder = nullptr;
		size_t writeFrame = 0;
		bool primed = false;
	};

	void mixFrame(size_t frameIndex, const int16_t *samples);

	static constexpr int ECHO_RATE = 16000;
	static constexpr int ECHO_DECIMATION = SAMPLE_RATE / ECHO_RATE;
	static constexpr int ECHO_FRAME_SAMPLES = FRAME_SAMPLES / ECHO_DECIMATION;

	void submitFrame(ndspWaveBuf &buf, const int16_t *src);

	bool running = false;
	std::atomic<bool> deafened{false};
	EchoCanceller *echo = nullptr;
	int16_t echoFrame[ECHO_FRAME_SAMPLES] = {};
	int16_t silentFrame[FRAME_SAMPLES] = {};
	int16_t *ring = nullptr;
	size_t playFrame = 0;
	size_t writtenFrames = 0;

	ndspWaveBuf waveBufs[4];
	int16_t *waveData = nullptr;
	int nextWaveBuf = 0;

	std::unordered_map<uint32_t, Speaker> speakers;
	std::mutex mutex;
};

} // namespace Discord

#endif // VOICE_AUDIO_H
