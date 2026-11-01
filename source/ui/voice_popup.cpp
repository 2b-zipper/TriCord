#include "ui/voice_popup.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/voice_client.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include "utils/redraw.h"

#include <algorithm>
#include <cmath>

namespace UI {

namespace {

constexpr float POPUP_W = 240.0f;
constexpr float POPUP_H = 116.0f;
constexpr float POPUP_X = (320.0f - POPUP_W) / 2.0f;
constexpr float POPUP_Y = (240.0f - POPUP_H) / 2.0f;

constexpr float BAR_X = POPUP_X + 44.0f;
constexpr float BAR_Y = POPUP_Y + 49.0f;
constexpr float BAR_W = 152.0f;
constexpr float BAR_H = 18.0f;

constexpr float MINUS_BTN_X = POPUP_X + 12.0f;
constexpr float PLUS_BTN_X = POPUP_X + 202.0f;
constexpr float STEP_BTN_Y = POPUP_Y + 48.0f;
constexpr float STEP_BTN_W = 26.0f;
constexpr float STEP_BTN_H = 20.0f;

constexpr float CLOSE_W = 70.0f;
constexpr float CLOSE_H = 22.0f;
constexpr float CLOSE_X = (320.0f - CLOSE_W) / 2.0f;
constexpr float CLOSE_Y = POPUP_Y + 82.0f;

} // namespace

VoicePopup &VoicePopup::getInstance() {
	static VoicePopup instance;
	return instance;
}

void VoicePopup::show(const Discord::VoiceParticipant &participant, bool isSelfUser) {
	userId = participant.userId;
	userName = participant.name;
	userAvatar = participant.avatar;
	isSelf = isSelfUser;
	open = true;
	Utils::requestRedraw();
}

void VoicePopup::render() {
	if (!open) {
		return;
	}

	drawOverlay(0.85f);
	drawPopupBackground(POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 0.90f, 8.0f);

	float avX = POPUP_X + 12.0f;
	float avY = POPUP_Y + 7.0f;
	float avSize = 16.0f;

	C3D_Tex *av = Discord::AvatarCache::getInstance().getAvatar(userId, userAvatar, "0");
	if (av) {
		Tex3DS_SubTexture sub = {(u16)av->width, (u16)av->height, 0.0f, 1.0f, 1.0f, 0.0f};
		C2D_Image img = {av, &sub};
		C2D_DrawImageAt(img, avX, avY, 0.92f, nullptr, avSize / av->width, avSize / av->height);
	} else {
		drawRoundedRect(avX, avY, 0.92f, avSize, avSize, 3.0f, ScreenManager::colorBackgroundLight());
	}

	std::string title = isSelf ? TR("voice.mic_sensitivity")
	                           : getTruncatedRichText(userName, 130.0f, 0.45f, 0.45f) + " " + TR("voice.user_volume");
	drawRichText(POPUP_X + 34.0f, POPUP_Y + 8.0f, 0.92f, 0.45f, 0.45f, ScreenManager::colorText(), title);

	C2D_DrawRectSolid(POPUP_X + 8.0f, POPUP_Y + 28.0f, 0.92f, POPUP_W - 16.0f, 1.0f, ScreenManager::colorSeparator());

	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();

	if (isSelf) {
		int sens = Config::getInstance().getMicSensitivity();
		std::string valStr = std::to_string(sens) + "%";
		drawCenteredText(POPUP_Y + 32.0f, 0.92f, 0.42f, 0.42f, ScreenManager::colorSelection(), valStr, 320.0f);

		drawRoundedRect(MINUS_BTN_X, STEP_BTN_Y, 0.92f, STEP_BTN_W, STEP_BTN_H, 4.0f,
		                ScreenManager::colorBackgroundLight());
		drawText(MINUS_BTN_X + 10.0f, STEP_BTN_Y + 1.0f, 0.93f, 0.50f, 0.50f, ScreenManager::colorText(), "-");

		drawRoundedRect(BAR_X, BAR_Y, 0.91f, BAR_W, BAR_H, 4.0f, ScreenManager::colorInput());

		int micPeak = voice.getMicLevel();
		float levelNorm = std::min(1.0f, (float)micPeak / 4000.0f);
		float fillW = BAR_W * levelNorm;
		int threshold = voice.getMicGateThreshold();
		bool active = micPeak >= threshold && !voice.isMuted();
		if (fillW > 2.0f) {
			drawRoundedRect(BAR_X, BAR_Y, 0.92f, fillW, BAR_H, 4.0f,
			                active ? C2D_Color32(59, 165, 93, 255) : C2D_Color32(40, 95, 55, 200));
		}

		float sensPos = BAR_X + BAR_W * ((float)sens / 100.0f);
		C2D_DrawRectSolid(sensPos - 1.0f, BAR_Y - 2.0f, 0.93f, 2.0f, BAR_H + 4.0f, ScreenManager::colorWhite());

		drawRoundedRect(PLUS_BTN_X, STEP_BTN_Y, 0.92f, STEP_BTN_W, STEP_BTN_H, 4.0f,
		                ScreenManager::colorBackgroundLight());
		drawText(PLUS_BTN_X + 8.0f, STEP_BTN_Y + 1.0f, 0.93f, 0.50f, 0.50f, ScreenManager::colorText(), "+");
	} else {
		int vol = voice.getUserVolume(userId);
		std::string valStr = std::to_string(vol) + "%";
		drawCenteredText(POPUP_Y + 32.0f, 0.92f, 0.42f, 0.42f, ScreenManager::colorSelection(), valStr, 320.0f);

		drawRoundedRect(MINUS_BTN_X, STEP_BTN_Y, 0.92f, STEP_BTN_W, STEP_BTN_H, 4.0f,
		                ScreenManager::colorBackgroundLight());
		drawText(MINUS_BTN_X + 10.0f, STEP_BTN_Y + 1.0f, 0.93f, 0.50f, 0.50f, ScreenManager::colorText(), "-");

		drawRoundedRect(BAR_X, BAR_Y, 0.91f, BAR_W, BAR_H, 4.0f, ScreenManager::colorInput());

		float ratio = std::clamp((float)vol / 200.0f, 0.0f, 1.0f);
		if (ratio > 0.01f) {
			drawRoundedRect(BAR_X, BAR_Y, 0.92f, BAR_W * ratio, BAR_H, 4.0f, ScreenManager::colorSelection());
		}

		C2D_DrawRectSolid(BAR_X + BAR_W * 0.5f - 0.5f, BAR_Y, 0.93f, 1.0f, BAR_H, C2D_Color32(255, 255, 255, 130));

		drawRoundedRect(PLUS_BTN_X, STEP_BTN_Y, 0.92f, STEP_BTN_W, STEP_BTN_H, 4.0f,
		                ScreenManager::colorBackgroundLight());
		drawText(PLUS_BTN_X + 8.0f, STEP_BTN_Y + 1.0f, 0.93f, 0.50f, 0.50f, ScreenManager::colorText(), "+");
	}

	drawRoundedRect(CLOSE_X, CLOSE_Y, 0.92f, CLOSE_W, CLOSE_H, 5.0f, ScreenManager::colorBackgroundLight());
	drawCenteredText(CLOSE_Y + 5.0f, 0.93f, 0.42f, 0.42f, ScreenManager::colorText(), TR("common.close"), 320.0f);
}

bool VoicePopup::handleTouch(const touchPosition &touch, bool isDown, bool isHeld) {
	if (!open) {
		return false;
	}

	if (isDown &&
	    (touch.px < POPUP_X || touch.px > POPUP_X + POPUP_W || touch.py < POPUP_Y || touch.py > POPUP_Y + POPUP_H)) {
		close();
		Utils::requestRedraw();
		return true;
	}

	if (isDown && touch.px >= CLOSE_X && touch.px <= CLOSE_X + CLOSE_W && touch.py >= CLOSE_Y &&
	    touch.py <= CLOSE_Y + CLOSE_H) {
		close();
		Utils::requestRedraw();
		return true;
	}

	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();

	if (isDown && touch.px >= MINUS_BTN_X && touch.px <= MINUS_BTN_X + STEP_BTN_W && touch.py >= STEP_BTN_Y &&
	    touch.py <= STEP_BTN_Y + STEP_BTN_H) {
		if (isSelf) {
			int sens = Config::getInstance().getMicSensitivity();
			Config::getInstance().setMicSensitivity(std::max(0, sens - 5));
		} else {
			int vol = voice.getUserVolume(userId);
			voice.setUserVolume(userId, std::max(0, vol - 10));
		}
		Utils::requestRedraw();
		return true;
	}

	if (isDown && touch.px >= PLUS_BTN_X && touch.px <= PLUS_BTN_X + STEP_BTN_W && touch.py >= STEP_BTN_Y &&
	    touch.py <= STEP_BTN_Y + STEP_BTN_H) {
		if (isSelf) {
			int sens = Config::getInstance().getMicSensitivity();
			Config::getInstance().setMicSensitivity(std::min(100, sens + 5));
		} else {
			int vol = voice.getUserVolume(userId);
			voice.setUserVolume(userId, std::min(200, vol + 10));
		}
		Utils::requestRedraw();
		return true;
	}

	if ((isDown || isHeld) && touch.px >= BAR_X && touch.px <= BAR_X + BAR_W && touch.py >= BAR_Y - 5.0f &&
	    touch.py <= BAR_Y + BAR_H + 5.0f) {
		float r = std::clamp(((float)touch.px - BAR_X) / BAR_W, 0.0f, 1.0f);
		if (isSelf) {
			Config::getInstance().setMicSensitivity((int)(r * 100.0f + 0.5f));
		} else {
			voice.setUserVolume(userId, (int)(r * 200.0f + 0.5f));
		}
		Utils::requestRedraw();
		return true;
	}

	return true;
}

bool VoicePopup::handleKeys(u32 kDown) {
	if (!open) {
		return false;
	}
	if (kDown & KEY_B) {
		close();
		Utils::requestRedraw();
		return true;
	}
	return false;
}

} // namespace UI
