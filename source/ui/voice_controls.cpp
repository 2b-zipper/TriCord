#include "ui/voice_controls.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "ui/voice_popup.h"
#include "utils/message_utils.h"

#include <algorithm>
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

std::vector<Discord::VoiceParticipant> getActiveParticipants(const std::string &fallbackChannelId) {
	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();
	Discord::DiscordClient &dc = Discord::DiscordClient::getInstance();

	if (voice.getState() != Discord::VoiceState::DISCONNECTED && !voice.getChannelId().empty()) {
		auto list = dc.getVoiceParticipants(voice.getChannelId());
		if (list.empty()) {
			const auto &user = dc.getCurrentUser();
			Discord::VoiceParticipant selfP;
			selfP.userId = user.id;
			selfP.name = user.global_name.empty() ? user.username : user.global_name;
			selfP.avatar = user.avatar;
			selfP.selfMute = voice.isMuted();
			selfP.selfDeaf = voice.isDeafened();
			selfP.mute = voice.isServerMuted();
			selfP.deaf = voice.isServerDeafened();
			list.push_back(selfP);
		}
		return list;
	}

	if (!fallbackChannelId.empty()) {
		return dc.getVoiceParticipants(fallbackChannelId);
	}

	return {};
}

void drawParticipantList(float y, const std::vector<Discord::VoiceParticipant> &participants, int maxRows) {
	drawText(10.0f, y, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(), TR("call.in_call"));
	y += 17.0f;

	constexpr float rowH = 26.0f;
	constexpr float avatarSize = 20.0f;
	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();

	int drawn = 0;
	for (const auto &p : participants) {
		if (drawn >= maxRows) {
			break;
		}

		float rowY = y + drawn * rowH;
		float avatarY = rowY + (rowH - avatarSize) / 2.0f;

		if (voice.isSpeaking(p.userId)) {
			drawCircle(14.0f + avatarSize / 2.0f, avatarY + avatarSize / 2.0f, 0.49f, avatarSize / 2.0f + 1.5f,
			           C2D_Color32(35, 165, 90, 255));
		}

		C3D_Tex *avatar = Discord::AvatarCache::getInstance().getAvatar(p.userId, p.avatar, "0");
		if (avatar) {
			Tex3DS_SubTexture sub = {(u16)avatar->width, (u16)avatar->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {avatar, &sub};
			C2D_DrawImageAt(img, 14.0f, avatarY, 0.5f, nullptr, avatarSize / avatar->width,
			                avatarSize / avatar->height);
		} else {
			drawRoundedRect(14.0f, avatarY, 0.5f, avatarSize, avatarSize, 4.0f, ScreenManager::colorBackgroundLight());
		}

		int stateIcons = (p.mute || p.selfMute ? 1 : 0) + (p.deaf || p.selfDeaf ? 1 : 0);
		float nameX = 14.0f + avatarSize + 6.0f;
		float nameLimit = 310.0f - nameX - stateIcons * 15.0f;
		drawRichText(nameX, rowY + 5.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorText(),
		             getTruncatedRichText(p.name, nameLimit, 0.45f, 0.45f));

		const char *micIcon = p.mute       ? "romfs:/discord-icons/mic-denied.png"
		                      : p.selfMute ? "romfs:/discord-icons/mic-muted.png"
		                                   : nullptr;
		const char *deafIcon = p.deaf       ? "romfs:/discord-icons/headphones-denied.png"
		                       : p.selfDeaf ? "romfs:/discord-icons/headphones-muted.png"
		                                    : nullptr;

		const float stateSize = 12.0f;
		float stateX = 310.0f - stateSize;
		const struct {
			const char *path;
			bool byServer;
		} icons[] = {{deafIcon, p.deaf}, {micIcon, p.mute}};

		for (const auto &entry : icons) {
			if (!entry.path) {
				continue;
			}
			C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage(entry.path);
			if (tex) {
				Tex3DS_SubTexture sub = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
				C2D_Image img = {tex, &sub};
				C2D_ImageTint tint;
				C2D_PlainImageTint(
				    &tint, entry.byServer ? ScreenManager::colorError() : ScreenManager::colorTextMuted(), 1.0f);
				C2D_DrawImageAt(img, stateX, rowY + (rowH - stateSize) / 2.0f, 0.5f, &tint, stateSize / tex->width,
				                stateSize / tex->height);
			}
			stateX -= stateSize + 3.0f;
		}

		drawn++;
	}

	if ((int)participants.size() > maxRows) {
		drawText(14.0f, y + maxRows * rowH, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		         "+" + std::to_string((int)participants.size() - maxRows));
	}
}

bool handleParticipantTouch(const touchPosition &touch, float startY,
                            const std::vector<Discord::VoiceParticipant> &participants, int maxRows) {
	constexpr float rowH = 26.0f;
	int count = std::min((int)participants.size(), maxRows);
	if (touch.px >= 10.0f && touch.px <= 310.0f && touch.py >= startY && touch.py < startY + count * rowH) {
		int idx = (touch.py - startY) / rowH;
		if (idx >= 0 && idx < (int)participants.size()) {
			const auto &p = participants[idx];
			std::string selfId = Discord::DiscordClient::getInstance().getCurrentUser().id;
			VoicePopup::getInstance().show(p, p.userId == selfId);
			return true;
		}
	}
	return false;
}

} // namespace VoiceControls

} // namespace UI
