#ifndef INCOMING_CALL_H
#define INCOMING_CALL_H

#include <citro2d.h>
#include <string>

namespace UI {

class IncomingCall {
  public:
	void update();
	void render();

	bool isVisible() const { return !channelId.empty(); }

  private:
	void accept();
	void decline();

	std::string channelId;
	std::string callerName;
	int selectedIndex = 0;
	int ringTimer = 0;
	bool ringPlaying = false;
};

} // namespace UI

#endif // INCOMING_CALL_H
