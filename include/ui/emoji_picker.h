#ifndef EMOJI_PICKER_H
#define EMOJI_PICKER_H

#include "discord/types.h"
#include <citro2d.h>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace UI {

class EmojiPicker {
  public:
	EmojiPicker();
	~EmojiPicker();

	void update(u32 kDown, u32 kHeld, u32 kUp, circlePosition circle);
	void render(C3D_RenderTarget *target, const Discord::Message *activeMsg);

	int getCurrentCategory() const { return currentCategoryIndex; }
	void reset();
	bool isClosed() const { return closed; }
	void setOnEmojiSelected(std::function<void(const std::string &)> callback) { onEmojiSelected = callback; }
	void setOnClosed(std::function<void()> callback) { onClosed = callback; }

	std::unordered_set<std::string> getVisibleTwemojisInChat(const std::vector<Discord::Message> &messages,
	                                                         float scrollY, float totalH);

  private:
	void handleCategoryChange(int delta);
	void ensureSelectionVisible();

	int currentCategoryIndex = 0;
	int emojiIndex = 0;
	float emojiScrollY = 0.0f;
	float targetEmojiScrollY = 0.0f;
	int emojisPerRow = 11;
	bool closed = false;

	int keyRepeatTimer = 0;
	static const int REPEAT_INITIAL_DELAY = 25;
	static const int REPEAT_INTERVAL = 8;

	bool isDragging = false;
	float lastTouchY = 0.0f;
	float touchVelocityY = 0.0f;
	float touchStartX = 0.0f;
	float touchStartY = 0.0f;
	int pendingTapIndex = -1;

	std::function<void(const std::string &)> onEmojiSelected;
	std::function<void()> onClosed;

	static const float CELL_SIZE;
	static const float EMOJI_SIZE;
	static const float HEADER_H;
};

} // namespace UI

#endif // EMOJI_PICKER_H
