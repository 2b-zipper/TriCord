#include "discord/echo_canceller.h"
#include "log.h"

#include "modules/audio_processing/aecm/echo_control_mobile.h"

namespace Discord {

namespace {
// Most aggressive of the five settings.
constexpr int16_t ECHO_MODE = 4;
constexpr int MAX_DELAY_MS = 500;
constexpr int MAX_LOGGED_FAILURES = 5;
} // namespace

bool EchoCanceller::start() {
	if (handle) {
		return true;
	}

	handle = webrtc::WebRtcAecm_Create();
	if (!handle) {
		Logger::log("[AEC] create failed");
		return false;
	}

	if (webrtc::WebRtcAecm_Init(handle, RATE) != 0) {
		Logger::log("[AEC] init failed");
		webrtc::WebRtcAecm_Free(handle);
		handle = nullptr;
		return false;
	}

	webrtc::AecmConfig config;
	config.cngMode = webrtc::AecmTrue;
	config.echoMode = ECHO_MODE;
	if (webrtc::WebRtcAecm_set_config(handle, config) != 0) {
		Logger::log("[AEC] set_config failed");
		webrtc::WebRtcAecm_Free(handle);
		handle = nullptr;
		return false;
	}

	delayMs = 0;
	failures = 0;
	Logger::log("[AEC] started (WebRTC AECM, echoMode=%d)", ECHO_MODE);
	return true;
}

void EchoCanceller::stop() {
	if (!handle) {
		return;
	}
	webrtc::WebRtcAecm_Free(handle);
	handle = nullptr;
	Logger::log("[AEC] stopped");
}

// State held from before a bypass describes an echo path that has since been
// unplugged, so re-initialise on the way back in.
void EchoCanceller::setEnabled(bool on) {
	if (enabled == on) {
		return;
	}
	enabled = on;
	if (on && handle && webrtc::WebRtcAecm_Init(handle, RATE) != 0) {
		Logger::log("[AEC] re-init failed");
		stop();
	}
	Logger::log("[AEC] %s", on ? "enabled" : "bypassed");
}

void EchoCanceller::bufferFarEnd(const int16_t *far, int count) {
	if (!handle || !enabled || count <= 0 || count % CHUNK != 0) {
		return;
	}

	for (int offset = 0; offset + CHUNK <= count; offset += CHUNK) {
		if (webrtc::WebRtcAecm_BufferFarend(handle, far + offset, CHUNK) != 0 && ++failures <= MAX_LOGGED_FAILURES) {
			Logger::log("[AEC] BufferFarend failed");
		}
	}
}

void EchoCanceller::process(int16_t *mic, int count) {
	if (!handle || !enabled || count <= 0 || count % CHUNK != 0) {
		return;
	}

	int hint = delayMs;
	if (hint < 0) {
		hint = 0;
	} else if (hint > MAX_DELAY_MS) {
		hint = MAX_DELAY_MS;
	}

	for (int offset = 0; offset + CHUNK <= count; offset += CHUNK) {
		int16_t out[CHUNK];
		if (webrtc::WebRtcAecm_Process(handle, mic + offset, nullptr, out, CHUNK, (int16_t)hint) != 0) {
			if (++failures <= MAX_LOGGED_FAILURES) {
				Logger::log("[AEC] Process failed");
			}
			continue;
		}
		for (int i = 0; i < CHUNK; i++) {
			mic[offset + i] = out[i];
		}
	}
}

} // namespace Discord
