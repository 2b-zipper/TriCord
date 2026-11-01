#ifndef VOICE_POPUP_H
#define VOICE_POPUP_H

#include "discord/types.h"
#include "discord/voice_client.h"
#include <3ds.h>
#include <string>

namespace UI {

class VoicePopup {
  public:
	static VoicePopup &getInstance();

	bool isOpen() const {
		return open && Discord::VoiceClient::getInstance().getState() != Discord::VoiceState::DISCONNECTED;
	}
	void show(const Discord::VoiceParticipant &participant, bool isSelf);
	void close() { open = false; }

	void render();
	bool handleTouch(const touchPosition &touch, bool isDown, bool isHeld);
	bool handleKeys(u32 kDown);

  private:
	VoicePopup() = default;
	bool open = false;
	std::string userId;
	std::string userName;
	std::string userAvatar;
	bool isSelf = false;
};

} // namespace UI

#endif // VOICE_POPUP_H
