#ifndef SOUND_PLAYER_H
#define SOUND_PLAYER_H

#include <3ds.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Utils {

enum class Sound {
	VOICE_JOIN,
	VOICE_LEFT,
	MIC_ON,
	MIC_OFF,
	HEADPHONE_ON,
	HEADPHONE_OFF,
	COUNT,
};

class SoundPlayer {
  public:
	static constexpr int SAMPLE_RATE = 32000;

	static SoundPlayer &getInstance();

	void init();
	void shutdown();

	void play(Sound sound);

  private:
	SoundPlayer() = default;

	SoundPlayer(const SoundPlayer &) = delete;
	SoundPlayer &operator=(const SoundPlayer &) = delete;

	struct Clip {
		int16_t *samples = nullptr;
		size_t count = 0;
	};

	bool load(Sound sound, const std::string &path);

	bool ready = false;
	Clip clips[(size_t)Sound::COUNT];
	ndspWaveBuf waveBufs[4];
	int nextWaveBuf = 0;
	std::mutex mutex;
};

} // namespace Utils

#endif // SOUND_PLAYER_H
