#ifndef ECHO_CANCELLER_H
#define ECHO_CANCELLER_H

#include <cstdint>

namespace Discord {

// Wraps WebRTC's AECM. Both sides are fed from the voice media thread, so
// nothing here locks.
class EchoCanceller {
  public:
	static constexpr int RATE = 16000;
	static constexpr int CHUNK = 160; // AECM only accepts 80 or 160
	static constexpr int MAX_DELAY_HINT_MS = 20;

	bool start();
	void stop();
	bool isRunning() const { return handle != nullptr; }

	void setEnabled(bool on);
	bool isEnabled() const { return enabled; }

	void bufferFarEnd(const int16_t *far, int count);
	void setDelayMs(int ms) { delayMs = ms < MAX_DELAY_HINT_MS ? ms : MAX_DELAY_HINT_MS; }
	void process(int16_t *mic, int count);

  private:
	void *handle = nullptr;
	bool enabled = true;
	int delayMs = 0;
	int failures = 0;
};

} // namespace Discord

#endif // ECHO_CANCELLER_H
