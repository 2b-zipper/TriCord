#include "ui/incoming_call.h"
#include "core/i18n.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include "utils/sound_player.h"

#include <3ds.h>

namespace UI {

namespace {
constexpr float SCREEN_W = 320.0f;
constexpr float SCREEN_H = 240.0f;
constexpr float CARD_W = 260.0f;
constexpr float CARD_H = 110.0f;
constexpr float CARD_X = (SCREEN_W - CARD_W) / 2.0f;
constexpr float CARD_Y = (SCREEN_H - CARD_H) / 2.0f;
constexpr float BTN_W = 108.0f;
constexpr float BTN_H = 32.0f;
constexpr float BTN_Y = CARD_Y + CARD_H - BTN_H - 12.0f;
constexpr float ACCEPT_X = CARD_X + 12.0f;
constexpr float DECLINE_X = CARD_X + CARD_W - BTN_W - 12.0f;
} // namespace

void IncomingCall::update() {
	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	std::string ringing = client.getIncomingCallChannel();

	if (ringing.empty()) {
		if (ringPlaying) {
			Utils::SoundPlayer::getInstance().stop();
			ringPlaying = false;
		}
		channelId.clear();
		callerName.clear();
		ringTimer = 0;
		return;
	}

	if (ringing != channelId) {
		channelId = ringing;
		selectedIndex = 0;
		ringTimer = 0;
		std::lock_guard<std::recursive_mutex> lock(client.getMutex());
		callerName = MessageUtils::getChannelDisplayName(client.getChannel(channelId));
	}

	if (--ringTimer <= 0) {
		Utils::SoundPlayer &sound = Utils::SoundPlayer::getInstance();
		sound.play(Utils::Sound::CALL_INCOMING);
		ringPlaying = true;
		ringTimer = sound.clipFrames(Utils::Sound::CALL_INCOMING);
		if (ringTimer <= 0) {
			ringTimer = 60;
		}
	}

	u32 kDown = hidKeysDown();

	if (kDown & (KEY_LEFT | KEY_RIGHT)) {
		selectedIndex = selectedIndex == 0 ? 1 : 0;
	}

	if (kDown & KEY_A) {
		if (selectedIndex == 0) {
			accept();
		} else {
			decline();
		}
		return;
	}

	if (kDown & KEY_B) {
		decline();
		return;
	}

	if (kDown & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);
		if (touch.py >= BTN_Y && touch.py <= BTN_Y + BTN_H) {
			if (touch.px >= ACCEPT_X && touch.px <= ACCEPT_X + BTN_W) {
				accept();
			} else if (touch.px >= DECLINE_X && touch.px <= DECLINE_X + BTN_W) {
				decline();
			}
		}
	}
}

void IncomingCall::accept() {
	std::string target = channelId;
	channelId.clear();
	if (ringPlaying) {
		Utils::SoundPlayer::getInstance().stop();
		ringPlaying = false;
	}
	Discord::VoiceClient::getInstance().connect("DM", target);
	Discord::DiscordClient::getInstance().stopRinging(target);
}

void IncomingCall::decline() {
	std::string target = channelId;
	channelId.clear();
	if (ringPlaying) {
		Utils::SoundPlayer::getInstance().stop();
		ringPlaying = false;
	}
	Discord::DiscordClient::getInstance().stopRinging(target);
}

void IncomingCall::render() {
	if (!isVisible()) {
		return;
	}

	drawOverlay(0.96f);

	drawRoundedRect(CARD_X, CARD_Y, 0.97f, CARD_W, CARD_H, 10.0f, ScreenManager::colorBackgroundDark());

	drawCenteredText(CARD_Y + 12.0f, 0.975f, 0.45f, 0.45f, ScreenManager::colorTextMuted(), TR("call.incoming"),
	                 SCREEN_W);

	drawCenteredRichText(CARD_Y + 32.0f, 0.975f, 0.6f, 0.6f, ScreenManager::colorText(),
	                     getTruncatedRichText(callerName, CARD_W - 20.0f, 0.6f, 0.6f), SCREEN_W);

	const struct {
		float x;
		const char *key;
		u32 color;
	} buttons[] = {{ACCEPT_X, "call.accept", ScreenManager::colorSuccess()},
	               {DECLINE_X, "call.decline", ScreenManager::colorError()}};

	for (int i = 0; i < 2; i++) {
		bool sel = (selectedIndex == i);
		drawRoundedRect(buttons[i].x, BTN_Y, 0.975f, BTN_W, BTN_H, 8.0f,
		                sel ? buttons[i].color : ScreenManager::colorBackgroundLight());
		std::string label = TR(buttons[i].key);
		float tw = measureRichText(label, 0.5f, 0.5f);
		drawRichText(buttons[i].x + (BTN_W - tw) / 2.0f, BTN_Y + 8.0f, 0.98f, 0.5f, 0.5f, ScreenManager::colorText(),
		             label);
	}
}

} // namespace UI
