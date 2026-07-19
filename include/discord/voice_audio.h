#ifndef VOICE_AUDIO_H
#define VOICE_AUDIO_H

#include <3ds.h>
#include <cstdint>
#include <map>
#include <mutex>
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

	void pump();

  private:
	struct Speaker {
		OpusDecoder *decoder = nullptr;
		size_t writeFrame = 0;
		bool primed = false;
	};

	void mixFrame(size_t frameIndex, const int16_t *samples);

	bool running = false;
	int16_t *ring = nullptr;
	size_t playFrame = 0;
	size_t writtenFrames = 0;

	ndspWaveBuf waveBufs[4];
	int16_t *waveData = nullptr;
	int nextWaveBuf = 0;

	std::map<uint32_t, Speaker> speakers;
	std::mutex mutex;
};

} // namespace Discord

#endif // VOICE_AUDIO_H
