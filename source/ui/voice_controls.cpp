#include "ui/voice_controls.h"
#include "discord/voice_client.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"

#include <string>

namespace UI {

namespace VoiceControls {

namespace {

bool inButton(const touchPosition &touch, float x, float y) {
	return touch.px >= x && touch.px < x + BUTTON_SIZE && touch.py >= y && touch.py < y + BUTTON_SIZE;
}

void drawButton(float x, float y, const char *iconPath, bool off) {
	drawRoundedRect(x, y, 0.54f, BUTTON_SIZE, BUTTON_SIZE, 8.0f, ScreenManager::colorBackgroundLight());

	C3D_Tex *tex = ImageManager::getInstance().getLocalImage(iconPath);
	if (!tex) {
		return;
	}

	Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
	C2D_Image img = {tex, &subtex};

	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, off ? ScreenManager::colorError() : ScreenManager::colorText(), 1.0f);

	const float iconSize = 20.0f;
	const float offset = (BUTTON_SIZE - iconSize) / 2.0f;
	C2D_DrawImageAt(img, x + offset, y + offset, 0.55f, &tint, iconSize / tex->width, iconSize / tex->height);
}

} // namespace

bool visible() { return Discord::VoiceClient::getInstance().getState() == Discord::VoiceState::ESTABLISHED; }

void draw(float x, float y) {
	if (!visible()) {
		return;
	}

	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();
	bool serverMuted = voice.isServerMuted();
	bool serverDeafened = voice.isServerDeafened();
	bool muted = voice.isMuted();
	bool deafened = voice.isDeafened();

	const char *micIcon = serverMuted ? "romfs:/discord-icons/mic-denied.png"
	                      : muted     ? "romfs:/discord-icons/mic-muted.png"
	                                  : "romfs:/discord-icons/mic.png";
	const char *deafIcon = serverDeafened ? "romfs:/discord-icons/headphones-denied.png"
	                       : deafened     ? "romfs:/discord-icons/headphones-muted.png"
	                                      : "romfs:/discord-icons/headphones.png";

	drawButton(x, y, micIcon, muted || serverMuted);
	drawButton(x + BUTTON_SIZE + BUTTON_GAP, y, deafIcon, deafened || serverDeafened);
	drawButton(x + (BUTTON_SIZE + BUTTON_GAP) * 2.0f, y, "romfs:/discord-icons/phone-hangup.png", false);
}

bool handleTouch(const touchPosition &touch, float x, float y) {
	if (!visible()) {
		return false;
	}

	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();
	const float deafX = x + BUTTON_SIZE + BUTTON_GAP;

	if (inButton(touch, x, y)) {
		if (!voice.isServerMuted()) {
			voice.setMuted(!voice.isMuted());
		}
		return true;
	}

	if (inButton(touch, deafX, y)) {
		if (!voice.isServerDeafened()) {
			voice.setDeafened(!voice.isDeafened());
		}
		return true;
	}

	if (inButton(touch, x + (BUTTON_SIZE + BUTTON_GAP) * 2.0f, y)) {
		voice.disconnect();
		return true;
	}

	return false;
}

} // namespace VoiceControls

} // namespace UI
