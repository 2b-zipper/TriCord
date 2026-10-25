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

// Playback side of a call.
//
// Everything is pulled by the DSP clock: packets are queued per speaker as they
// arrive (still Opus, a few hundred bytes each) and decoded only when the next
// 20 ms is actually needed. That has three effects on a CPU as slow as the Old
// 3DS/2DS one:
//   - latency is bounded (the queue is capped and skips ahead after a burst);
//   - a lost or late packet is concealed instead of shifting everything behind
//     it, so delay never builds up;
//   - nothing is decoded that would be thrown away.
class VoiceAudio {
  public:
	// 16 kHz mono. The echo canceller works at 16 kHz as well, so the far-end
	// reference needs no resampling, and the DSP does the up-conversion itself.
	static constexpr int SAMPLE_RATE = 16000;
	static constexpr int FRAME_MS = 20;
	static constexpr int FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;

	bool start();
	void stop();
	bool isRunning() const { return running; }

	// seq is the RTP sequence number; it orders packets and reveals losses.
	void pushOpus(uint32_t ssrc, uint16_t seq, const uint8_t *data, size_t len);
	void dropSpeaker(uint32_t ssrc);

	void setDeafened(bool d);
	bool isDeafened() const { return deafened; }

	void setEchoCanceller(EchoCanceller *e) { echo = e; }

	// Keeps the DSP queue topped up. Call at least every ~10 ms.
	void pump();

	// Smoothed peak of what the speaker is playing (0..32767). Used to raise the
	// transmit threshold while the far side is talking.
	int farLevel() const { return level; }

	// Logs and resets the counters; for the periodic call statistics.
	void logStats();

  private:
	static constexpr int OUT_BUFFERS = 3;    // wave buffers in the ring
	static constexpr int OUT_QUEUE = 2;      // frames kept queued in the DSP
	static constexpr int QUEUE_SLOTS = 8;    // packets held per speaker
	static constexpr int MAX_PACKET = 512;   // bytes; a 20 ms Opus frame is far smaller
	static constexpr int JITTER_START = 2;   // packets gathered before a speaker starts
	static constexpr int JITTER_HIGH = 5;    // deeper than this and we skip ahead
	static constexpr int JITTER_KEEP = 3;    // depth left after skipping
	static constexpr int MAX_CONCEAL = 2;    // consecutive concealed frames before rebuffering
	static constexpr int MAX_GAP_CONCEAL = 2; // lost packets worth concealing in one go
	static constexpr int IDLE_EVICT_SECONDS = 15;
	static_assert(FRAME_SAMPLES % EchoCanceller::CHUNK == 0, "a frame must be a whole number of AEC chunks");
	static_assert(JITTER_HIGH < QUEUE_SLOTS, "queue must be able to hold JITTER_HIGH + 1 packets");

	struct Speaker {
		OpusDecoder *decoder = nullptr;
		uint16_t lastSeq = 0;
		bool haveSeq = false;
		bool playing = false;       // false while (re)buffering
		bool arrivedSilence = true; // last queued packet was an Opus silence frame
		bool playedSilence = true;  // last played packet was an Opus silence frame
		uint8_t head = 0;
		uint8_t count = 0;
		uint8_t concealed = 0;
		uint8_t idleSeconds = 0;
		uint16_t len[QUEUE_SLOTS];  // 0 marks a lost packet to conceal
		uint8_t data[QUEUE_SLOTS][MAX_PACKET];
	};

	struct Stats {
		uint32_t played = 0;
		uint32_t concealed = 0;
		uint32_t underruns = 0;
		uint32_t skipped = 0;
		uint32_t late = 0;
	};

	static void enqueue(Speaker &s, const uint8_t *data, size_t len, Stats &stats);
	static void destroy(Speaker &s);
	void renderFrame();
	void submitFrame(ndspWaveBuf &buf);
	void housekeeping();

	bool running = false;
	std::atomic<bool> deafened{false};
	std::atomic<int> level{0};
	EchoCanceller *echo = nullptr;

	ndspWaveBuf waveBufs[OUT_BUFFERS];
	int16_t *waveData = nullptr;
	int nextWaveBuf = 0;
	int16_t frame[FRAME_SAMPLES] = {};
	unsigned ticks = 0;

	Stats stats;
	std::unordered_map<uint32_t, std::unique_ptr<Speaker>> speakers;
	std::mutex mutex;
};

} // namespace Discord

#endif // VOICE_AUDIO_H
