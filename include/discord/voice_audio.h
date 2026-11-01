#ifndef VOICE_AUDIO_H
#define VOICE_AUDIO_H

#include "discord/echo_canceller.h"

#include <3ds.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

struct OpusDecoder;

namespace Discord {

class VoiceAudio {
  public:
	// Same rate as the echo canceller, so the far-end reference needs no resampling.
	static constexpr int SAMPLE_RATE = 16000;
	static constexpr int FRAME_MS = 20;
	static constexpr int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;

	bool start();
	void stop();
	bool isRunning() const { return running; }

	void pushOpus(uint32_t ssrc, uint16_t seq, const uint8_t *data, size_t len, float volume = 1.0f);
	void dropSpeaker(uint32_t ssrc);
	void setSpeakerVolume(uint32_t ssrc, float volume);

	void setDeafened(bool d);
	bool isDeafened() const { return deafened; }

	void setEchoCanceller(EchoCanceller *e) { echo = e; }

	void pump();

	int farLevel() const { return level; }

	void logStats();

  private:
	static constexpr int OUT_BUFFERS = 4;
	static constexpr int QUEUE_SLOTS = 8;
	static constexpr int MAX_PACKET = 512;
	static constexpr int JITTER_START = 2;
	static constexpr int JITTER_HIGH = 5;
	static constexpr int JITTER_KEEP = 3;
	static constexpr int MAX_CONCEAL = 2;
	static constexpr int MAX_GAP_CONCEAL = 2;
	static constexpr int IDLE_EVICT_SECONDS = 15;
	static_assert(FRAME_SAMPLES % EchoCanceller::CHUNK == 0, "a frame must be a whole number of AEC chunks");
	static_assert(JITTER_HIGH < QUEUE_SLOTS, "queue must be able to hold JITTER_HIGH + 1 packets");

	struct Speaker {
		OpusDecoder *decoder = nullptr;
		float volume = 1.0f;
		uint16_t lastSeq = 0;
		bool haveSeq = false;
		bool playing = false;
		bool arrivedSilence = true;
		bool playedSilence = true;
		uint8_t head = 0;
		uint8_t count = 0;
		uint8_t concealed = 0;
		uint8_t idleSeconds = 0;
		uint16_t len[QUEUE_SLOTS]; // 0 marks a lost packet to conceal
		uint8_t data[QUEUE_SLOTS][MAX_PACKET];
	};

	struct Stats {
		uint32_t played = 0;
		uint32_t concealed = 0;
		uint32_t underruns = 0;
		uint32_t dry = 0;
		uint32_t skipped = 0;
		uint32_t late = 0;
	};

	static void enqueue(Speaker &s, const uint8_t *data, size_t len, Stats &stats);
	static void destroy(Speaker &s);
	void renderFrame(int16_t *out);
	void submitFrame(ndspWaveBuf &buf);
	void housekeeping();

	bool running = false;
	std::atomic<bool> deafened{false};
	std::atomic<int> level{0};
	EchoCanceller *echo = nullptr;

	ndspWaveBuf waveBufs[OUT_BUFFERS];
	int16_t *waveData = nullptr;
	int nextWaveBuf = 0;
	int outQueue = 2;
	unsigned ticks = 0;

	Stats stats;
	std::unordered_map<uint32_t, std::unique_ptr<Speaker>> speakers;
	std::mutex mutex;
};

} // namespace Discord

#endif // VOICE_AUDIO_H
