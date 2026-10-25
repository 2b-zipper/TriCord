#include "discord/voice_audio.h"
#include "log.h"

#include <cstring>
#include <malloc.h>
#include <new>
#include <opus/opus.h>

namespace Discord {

namespace {
constexpr int NDSP_CHANNEL = 0;

bool isOpusSilence(const uint8_t *d, size_t n) { return n == 3 && d[0] == 0xF8 && d[1] == 0xFF && d[2] == 0xFE; }

bool slotBusy(const ndspWaveBuf &b) { return b.status != NDSP_WBUF_FREE && b.status != NDSP_WBUF_DONE; }
} // namespace

bool VoiceAudio::start() {
	std::lock_guard<std::mutex> lock(mutex);
	if (running) {
		return true;
	}

	waveData = (int16_t *)linearAlloc(OUT_BUFFERS * FRAME_SAMPLES * sizeof(int16_t));
	if (!waveData) {
		Logger::log("[VoiceAudio] allocation failed");
		return false;
	}

	ndspChnReset(NDSP_CHANNEL);
	// The DSP resamples 16 kHz up to its native rate; linear keeps the hold
	// artefacts of "none" out of the voice.
	ndspChnSetInterp(NDSP_CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(NDSP_CHANNEL, (float)SAMPLE_RATE);
	ndspChnSetFormat(NDSP_CHANNEL, NDSP_FORMAT_MONO_PCM16);

	float mix[12];
	memset(mix, 0, sizeof(mix));
	mix[0] = 1.0f;
	mix[1] = 1.0f;
	ndspChnSetMix(NDSP_CHANNEL, mix);

	memset(waveBufs, 0, sizeof(waveBufs));
	for (int i = 0; i < OUT_BUFFERS; i++) {
		waveBufs[i].data_vaddr = waveData + (size_t)i * FRAME_SAMPLES;
		waveBufs[i].nsamples = FRAME_SAMPLES;
		waveBufs[i].status = NDSP_WBUF_DONE;
	}

	nextWaveBuf = 0;
	ticks = 0;
	level = 0;
	stats = Stats();
	running = true;
	Logger::log("[VoiceAudio] started (%d Hz, queue %d x %d ms)", SAMPLE_RATE, OUT_QUEUE, FRAME_MS);
	return true;
}

void VoiceAudio::stop() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running) {
		return;
	}

	ndspChnWaveBufClear(NDSP_CHANNEL);

	for (auto &pair : speakers) {
		destroy(*pair.second);
	}
	speakers.clear();

	linearFree(waveData);
	waveData = nullptr;
	running = false;
	Logger::log("[VoiceAudio] stopped");
}

void VoiceAudio::destroy(Speaker &s) {
	if (s.decoder) {
		opus_decoder_destroy(s.decoder);
		s.decoder = nullptr;
	}
}

void VoiceAudio::enqueue(Speaker &s, const uint8_t *data, size_t len, Stats &stats) {
	if (s.count == QUEUE_SLOTS) {
		s.head = (uint8_t)((s.head + 1) % QUEUE_SLOTS);
		s.count--;
		stats.skipped++;
	}
	const int slot = (s.head + s.count) % QUEUE_SLOTS;
	s.len[slot] = (uint16_t)len;
	if (len) {
		memcpy(s.data[slot], data, len);
	}
	s.count++;
}

void VoiceAudio::pushOpus(uint32_t ssrc, uint16_t seq, const uint8_t *data, size_t len) {
	if (len == 0 || len > MAX_PACKET) {
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);
	if (!running || deafened) {
		return;
	}

	auto &slot = speakers[ssrc];
	if (!slot) {
		slot.reset(new (std::nothrow) Speaker());
		if (!slot) {
			speakers.erase(ssrc);
			Logger::log("[VoiceAudio] out of memory for ssrc %lu", (unsigned long)ssrc);
			return;
		}
	}
	Speaker &s = *slot;

	if (!s.decoder) {
		int err = 0;
		s.decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
		if (err != OPUS_OK || !s.decoder) {
			Logger::log("[VoiceAudio] opus_decoder_create failed for ssrc %lu: %d", (unsigned long)ssrc, err);
			s.decoder = nullptr;
			speakers.erase(ssrc);
			return;
		}
		Logger::log("[VoiceAudio] new speaker ssrc=%lu", (unsigned long)ssrc);
	}

	s.idleSeconds = 0;

	if (s.haveSeq) {
		const int delta = (int16_t)(uint16_t)(seq - s.lastSeq);
		// A packet at or behind the last one is a duplicate, or it arrived after
		// its slot was already played or concealed. A big jump backwards means the
		// sender restarted its counter, which is treated as a fresh stream.
		if (delta <= 0 && delta > -64) {
			stats.late++;
			return;
		}
		// Packets known to be missing get a placeholder so the decoder conceals
		// them in order. After a silence frame the gap is only the sender having
		// stopped, not a loss.
		if (delta > 1 && delta <= 1 + MAX_GAP_CONCEAL && s.playing && !s.arrivedSilence) {
			for (int i = 1; i < delta; i++) {
				enqueue(s, nullptr, 0, stats);
			}
		}
	}
	s.lastSeq = seq;
	s.haveSeq = true;
	s.arrivedSilence = isOpusSilence(data, len);

	enqueue(s, data, len, stats);

	// A burst after a stall would otherwise sit in the queue as permanent delay:
	// playback consumes exactly as fast as the sender produces.
	if (s.count > JITTER_HIGH) {
		const int drop = s.count - JITTER_KEEP;
		s.head = (uint8_t)((s.head + drop) % QUEUE_SLOTS);
		s.count = JITTER_KEEP;
		stats.skipped += (uint32_t)drop;
	}
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

	for (auto &pair : speakers) {
		Speaker &s = *pair.second;
		s.head = 0;
		s.count = 0;
		s.playing = false;
		s.concealed = 0;
	}
}

void VoiceAudio::dropSpeaker(uint32_t ssrc) {
	std::lock_guard<std::mutex> lock(mutex);
	auto it = speakers.find(ssrc);
	if (it == speakers.end()) {
		return;
	}
	destroy(*it->second);
	speakers.erase(it);
}

// Builds the next 20 ms by pulling one packet from every speaker that is due.
void VoiceAudio::renderFrame() {
	int32_t acc[FRAME_SAMPLES];
	bool any = false;

	for (auto &pair : speakers) {
		Speaker &s = *pair.second;
		if (!s.decoder) {
			continue;
		}

		if (!s.playing) {
			if (s.count < JITTER_START) {
				continue;
			}
			s.playing = true;
			s.concealed = 0;
		}

		const uint8_t *packet = nullptr;
		size_t packetLen = 0;
		if (s.count > 0) {
			packetLen = s.len[s.head];
			packet = packetLen ? s.data[s.head] : nullptr;
		} else if (s.playedSilence || s.concealed >= MAX_CONCEAL) {
			// The talk spurt ended (or the sender is gone): rebuffer instead of
			// inventing audio.
			s.playing = false;
			continue;
		} else {
			stats.underruns++;
		}

		int16_t pcm[FRAME_SAMPLES];
		const int decoded = opus_decode(s.decoder, packet, (opus_int32)packetLen, pcm, FRAME_SAMPLES, 0);

		if (s.count > 0) {
			s.head = (uint8_t)((s.head + 1) % QUEUE_SLOTS);
			s.count--;
		}
		if (packetLen == 0) {
			s.concealed++;
			stats.concealed++;
		} else {
			s.concealed = 0;
			s.playedSilence = isOpusSilence(packet, packetLen);
			stats.played++;
		}

		if (decoded <= 0) {
			continue;
		}
		if (decoded < FRAME_SAMPLES) {
			memset(pcm + decoded, 0, (FRAME_SAMPLES - decoded) * sizeof(int16_t));
		}

		if (!any) {
			for (int i = 0; i < FRAME_SAMPLES; i++) {
				acc[i] = pcm[i];
			}
			any = true;
		} else {
			for (int i = 0; i < FRAME_SAMPLES; i++) {
				acc[i] += pcm[i];
			}
		}
	}

	int peak = 0;
	if (any) {
		for (int i = 0; i < FRAME_SAMPLES; i++) {
			int32_t v = acc[i];
			if (v > 32767) {
				v = 32767;
			} else if (v < -32768) {
				v = -32768;
			}
			frame[i] = (int16_t)v;
			const int m = v < 0 ? -v : v;
			if (m > peak) {
				peak = m;
			}
		}
	} else {
		memset(frame, 0, sizeof(frame));
	}

	int lvl = level - (level >> 3);
	level = peak > lvl ? peak : lvl;
}

void VoiceAudio::submitFrame(ndspWaveBuf &buf) {
	int16_t *dst = (int16_t *)buf.data_vaddr;
	memcpy(dst, frame, sizeof(frame));
	DSP_FlushDataCache(dst, sizeof(frame));
	ndspChnWaveBufAdd(NDSP_CHANNEL, &buf);

	// The reference has to advance in step with what the DSP is given, silence
	// included, or the delay estimate breaks every time the channel goes quiet.
	if (echo) {
		echo->bufferFarEnd(frame, FRAME_SAMPLES);
	}

	nextWaveBuf = (nextWaveBuf + 1) % OUT_BUFFERS;
}

void VoiceAudio::housekeeping() {
	// Once a second: a speaker that has been silent for a while gives its decoder
	// (about 20 KB) back.
	for (auto it = speakers.begin(); it != speakers.end();) {
		Speaker &s = *it->second;
		if (!s.playing && s.count == 0 && ++s.idleSeconds >= IDLE_EVICT_SECONDS) {
			destroy(s);
			it = speakers.erase(it);
		} else {
			++it;
		}
	}
}

void VoiceAudio::pump() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running) {
		return;
	}

	int queued = 0;
	for (const ndspWaveBuf &b : waveBufs) {
		if (slotBusy(b)) {
			queued++;
		}
	}

	// Refilled only up to OUT_QUEUE frames, never in a burst: a deeper queue is
	// pure latency, and a burst of catch-up frames would knock the echo
	// canceller's far-end timeline out of step with the microphone.
	while (queued < OUT_QUEUE) {
		ndspWaveBuf &buf = waveBufs[nextWaveBuf];
		if (slotBusy(buf)) {
			break;
		}
		renderFrame();
		submitFrame(buf);
		queued++;

		if (++ticks % (1000 / FRAME_MS) == 0) {
			housekeeping();
		}
	}

	// Only the queue is reported. The DSP pipeline and acoustic path are left out
	// on purpose: AECM recovers from an underestimated delay but not from an
	// overestimated one.
	if (echo) {
		echo->setDelayMs(queued * FRAME_MS);
	}
}

void VoiceAudio::logStats() {
	std::lock_guard<std::mutex> lock(mutex);
	if (!running) {
		return;
	}
	Logger::log("[VoiceAudio] speakers=%u played=%lu concealed=%lu underrun=%lu skipped=%lu late=%lu",
	            (unsigned)speakers.size(), (unsigned long)stats.played, (unsigned long)stats.concealed,
	            (unsigned long)stats.underruns, (unsigned long)stats.skipped, (unsigned long)stats.late);
	stats = Stats();
}

} // namespace Discord
