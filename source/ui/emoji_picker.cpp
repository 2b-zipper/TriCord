#include "ui/emoji_picker.h"
#include "ui/emoji_manager.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/utf8_utils.h"
#include <algorithm>
#include <cmath>

namespace UI {

const float EmojiPicker::CELL_SIZE = 28.0f;
const float EmojiPicker::EMOJI_SIZE = 22.0f;
const float EmojiPicker::HEADER_H = 35.0f;

EmojiPicker::EmojiPicker() { reset(); }
EmojiPicker::~EmojiPicker() {}

void EmojiPicker::reset() {
	currentCategoryIndex = 0;
	emojiIndex = 0;
	emojiScrollY = 0.0f;
	targetEmojiScrollY = 0.0f;
	closed = false;
	isDragging = false;
	touchVelocityY = 0.0f;
	pendingTapIndex = -1;
	keyRepeatTimer = 0;
}

void EmojiPicker::handleCategoryChange(int delta) {
	auto &emojiMgr = EmojiManager::getInstance();
	const auto &categories = emojiMgr.getCategories();
	if (categories.empty()) {
		return;
	}
	currentCategoryIndex = (currentCategoryIndex + categories.size() + delta) % categories.size();
	emojiIndex = 0;
	targetEmojiScrollY = 0.0f;
	emojiScrollY = 0.0f;
}

void EmojiPicker::update(u32 kDown, u32 kHeld, u32 kUp, circlePosition circle) {
	auto &emojiMgr = EmojiManager::getInstance();
	const auto &categories = emojiMgr.getCategories();
	if (categories.empty()) {
		return;
	}

	const auto &currentCat = categories[currentCategoryIndex];
	const float VISIBLE_H = 240.0f - HEADER_H;
	int rowsCount = (currentCat.emojiIndices.size() + emojisPerRow - 1) / emojisPerRow;
	float totalH = rowsCount * CELL_SIZE;

	touchPosition touch;
	hidTouchRead(&touch);

	if (kDown & KEY_TOUCH) {
		if (touch.py < HEADER_H) {
			int catIdx = touch.px / (320.0f / 8.0f);
			if (catIdx >= 0 && catIdx < 8 && catIdx != currentCategoryIndex) {
				currentCategoryIndex = catIdx;
				emojiIndex = 0;
				targetEmojiScrollY = 0.0f;
				emojiScrollY = 0.0f;
				pendingTapIndex = -1;
			}
		} else {
			isDragging = true;
			lastTouchY = touch.py;
			touchStartX = touch.px;
			touchStartY = touch.py;
			touchVelocityY = 0.0f;
		}
	} else if (kHeld & KEY_TOUCH) {
		if (isDragging) {
			float deltaY = touch.py - lastTouchY;
			targetEmojiScrollY -= deltaY;
			touchVelocityY = deltaY;
			lastTouchY = touch.py;
		}
	} else if (kUp & KEY_TOUCH) {
		if (isDragging) {
			isDragging = false;
			float totalDrag = std::abs(touchStartY - lastTouchY);
			if (totalDrag < 30.0f) {
				touchVelocityY = 0.0f;
				float gridW = emojisPerRow * CELL_SIZE;
				float xOffset = (320.0f - gridW) / 2.0f;
				int col = (int)((touchStartX - xOffset) / CELL_SIZE);
				int row = (int)((touchStartY - HEADER_H + emojiScrollY) / CELL_SIZE);
				if (col >= 0 && col < emojisPerRow && row >= 0) {
					int idx = row * emojisPerRow + col;
					if (idx >= 0 && idx < (int)currentCat.emojiIndices.size()) {
						if (idx == pendingTapIndex) {
							if (onEmojiSelected) {
								const auto &allCodepoints = emojiMgr.getCodepoints();
								onEmojiSelected(Utils::Utf8::hexToUtf8(allCodepoints[currentCat.emojiIndices[idx]]));
							}
							pendingTapIndex = -1;
						} else {
							emojiIndex = idx;
							pendingTapIndex = idx;
						}
					}
				}
			}
		}
	}

	if (!isDragging) {
		targetEmojiScrollY -= touchVelocityY;
		touchVelocityY *= 0.85f;
	}

	if (abs(circle.dy) > 35) {
		targetEmojiScrollY -= circle.dy * 0.08f;
	}

	if (kDown & KEY_R) {
		handleCategoryChange(1);
	} else if (kDown & KEY_L) {
		handleCategoryChange(-1);
	}

	if (kDown & KEY_B) {
		closed = true;
		if (onClosed) {
			onClosed();
		}
		return;
	}

	bool up = false, down = false, left = false, right = false;
	if (kDown & KEY_UP) {
		up = true;
		keyRepeatTimer = 0;
	}
	if (kDown & KEY_DOWN) {
		down = true;
		keyRepeatTimer = 0;
	}
	if (kDown & KEY_LEFT) {
		left = true;
		keyRepeatTimer = 0;
	}
	if (kDown & KEY_RIGHT) {
		right = true;
		keyRepeatTimer = 0;
	}

	if (kHeld & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
		keyRepeatTimer++;
		if (keyRepeatTimer >= REPEAT_INITIAL_DELAY && (keyRepeatTimer - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0) {
			if (kHeld & KEY_UP) {
				up = true;
			}
			if (kHeld & KEY_DOWN) {
				down = true;
			}
			if (kHeld & KEY_LEFT) {
				left = true;
			}
			if (kHeld & KEY_RIGHT) {
				right = true;
			}
		}
	} else {
		keyRepeatTimer = 0;
	}

	if (up || down || left || right) {
		int firstVisibleRow = (int)(targetEmojiScrollY / CELL_SIZE);
		int lastVisibleRow = (int)((targetEmojiScrollY + VISIBLE_H - 1) / CELL_SIZE);
		int selRow = emojiIndex / emojisPerRow;
		int selCol = emojiIndex % emojisPerRow;

		if (selRow < firstVisibleRow || selRow > lastVisibleRow) {
			int snapRow = (selRow < firstVisibleRow) ? firstVisibleRow : lastVisibleRow;
			emojiIndex = snapRow * emojisPerRow + selCol;
		} else {
			if (up) {
				emojiIndex -= emojisPerRow;
			}
			if (down) {
				emojiIndex += emojisPerRow;
			}
			if (left) {
				emojiIndex--;
			}
			if (right) {
				emojiIndex++;
			}
			ensureSelectionVisible();
		}

		if (emojiIndex < 0) {
			emojiIndex = 0;
		}
		if (emojiIndex >= (int)currentCat.emojiIndices.size()) {
			emojiIndex = (int)currentCat.emojiIndices.size() - 1;
		}
	}

	if (kDown & KEY_A && onEmojiSelected) {
		const auto &allCodepoints = emojiMgr.getCodepoints();
		onEmojiSelected(Utils::Utf8::hexToUtf8(allCodepoints[currentCat.emojiIndices[emojiIndex]]));
	}

	if (targetEmojiScrollY < 0) {
		targetEmojiScrollY = 0;
		touchVelocityY = 0.0f;
	}
	if (totalH > VISIBLE_H && targetEmojiScrollY > totalH - VISIBLE_H) {
		targetEmojiScrollY = totalH - VISIBLE_H;
		touchVelocityY = 0.0f;
	}

	emojiScrollY += (targetEmojiScrollY - emojiScrollY) * 0.25f;
}

void EmojiPicker::ensureSelectionVisible() {
	int row = emojiIndex / emojisPerRow;
	const float VISIBLE_H = 240.0f - HEADER_H;
	float yStart = row * CELL_SIZE;
	float yEnd = yStart + CELL_SIZE;

	if (yStart < targetEmojiScrollY) {
		targetEmojiScrollY = yStart;
	} else if (yEnd > targetEmojiScrollY + VISIBLE_H) {
		targetEmojiScrollY = yEnd - VISIBLE_H;
	}
}

void EmojiPicker::render(C3D_RenderTarget *target, const Discord::Message *activeMsg) {
	auto &emojiMgr = EmojiManager::getInstance();
	const auto &categories = emojiMgr.getCategories();
	const auto &allCodepoints = emojiMgr.getCodepoints();

	if (categories.empty()) {
		return;
	}

	drawRoundedRect(0, 0, 0.45f, 320, HEADER_H, 0.0f, ScreenManager::colorBackgroundLight());

	const float tabW = 320.0f / 8.0f;
	const char *tabIcons[] = {
	    "romfs:/discord-icons/reaction.png", "romfs:/discord-icons/nature.png",
	    "romfs:/discord-icons/food.png",     "romfs:/discord-icons/gamecontroller.png",
	    "romfs:/discord-icons/bicycle.png",  "romfs:/discord-icons/object.png",
	    "romfs:/discord-icons/heart.png",    "romfs:/discord-icons/flag.png",
	};

	for (int i = 0; i < 8; i++) {
		C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage(tabIcons[i]);
		if (tex) {
			float iconSize = 16.0f;
			Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {tex, &subtex};
			C2D_ImageTint tint;
			C2D_PlainImageTint(
			    &tint, (i == currentCategoryIndex) ? ScreenManager::colorWhite() : ScreenManager::colorText(), 1.0f);
			C2D_DrawImageAt(img, i * tabW + (tabW - iconSize) / 2.0f, 9.0f, 0.47f, &tint, iconSize / tex->width,
			                iconSize / tex->height);
		}
		if (i == currentCategoryIndex) {
			drawRoundedRect(i * tabW, HEADER_H - 2, 0.47f, tabW, 2, 0.0f, ScreenManager::colorSelection());
		}
	}

	const auto &currentCat = categories[currentCategoryIndex];
	const float VISIBLE_H = 240.0f - HEADER_H;
	float gridW = emojisPerRow * CELL_SIZE;
	float xOffset = (320.0f - gridW) / 2.0f;

	int minRow = std::max(0, (int)(emojiScrollY / CELL_SIZE) - 1);
	int maxRow = (int)((emojiScrollY + VISIBLE_H) / CELL_SIZE) + 2;
	size_t startIndex = (size_t)minRow * emojisPerRow;
	size_t endIndex = std::min(currentCat.emojiIndices.size(), (size_t)maxRow * emojisPerRow);

	for (size_t i = startIndex; i < endIndex; i++) {
		size_t globalIdx = currentCat.emojiIndices[i];
		size_t row = i / (size_t)emojisPerRow;
		size_t col = i % (size_t)emojisPerRow;
		float x = xOffset + (float)col * CELL_SIZE;
		float y = HEADER_H + (float)row * CELL_SIZE - emojiScrollY;

		if (y + CELL_SIZE < HEADER_H || y > 240) {
			continue;
		}

		bool isReacted = false;
		if (activeMsg) {
			std::string emojiUtf8 = Utils::Utf8::hexToUtf8(allCodepoints[globalIdx]);
			for (const auto &r : activeMsg->reactions) {
				if (r.me && r.emoji.name == emojiUtf8) {
					isReacted = true;
					break;
				}
			}
		}

		bool isSelected = (i == (size_t)emojiIndex);
		if (isSelected) {
			drawRoundedRect(x + 1, y + 1, 0.41f, CELL_SIZE - 2, CELL_SIZE - 2, 4.0f, C2D_Color32(110, 110, 115, 255));
		}

		if (isReacted) {
			float pad = isSelected ? 3.0f : 2.0f;
			drawRoundedRect(x + pad, y + pad, isSelected ? 0.415f : 0.405f, CELL_SIZE - pad * 2, CELL_SIZE - pad * 2,
			                3.0f, ScreenManager::colorSelection() & 0xFFFFFF88);
		}

		EmojiManager::EmojiInfo info = emojiMgr.getTwemojiInfo(allCodepoints[globalIdx]);
		if (info.tex) {
			float scale = EMOJI_SIZE / info.originalW;
			float dX = x + (CELL_SIZE - EMOJI_SIZE) / 2.0f;
			float dY = y + (CELL_SIZE - EMOJI_SIZE) / 2.0f;
			Tex3DS_SubTexture sub = {(u16)info.originalW,
			                         (u16)info.originalH,
			                         0.0f,
			                         1.0f,
			                         (float)info.originalW / info.tex->width,
			                         1.0f - (float)info.originalH / info.tex->height};
			C2D_DrawImageAt({info.tex, &sub}, dX, dY, 0.42f, nullptr, scale, scale);
		} else {
			drawRoundedRect(x + 5, y + 5, 0.415f, CELL_SIZE - 10, CELL_SIZE - 10, 4.0f, C2D_Color32(50, 50, 55, 255));
		}
	}

	int rowsCount = (currentCat.emojiIndices.size() + emojisPerRow - 1) / emojisPerRow;
	float totalH = rowsCount * CELL_SIZE;
	if (totalH > VISIBLE_H) {
		float barH = std::max(10.0f, (VISIBLE_H / totalH) * VISIBLE_H);
		float barY = HEADER_H + (emojiScrollY / (totalH - VISIBLE_H)) * (VISIBLE_H - barH);
		drawRoundedRect(316, barY, 0.49f, 3, barH, 1.5f, C2D_Color32(150, 150, 150, 120));
	}
}

} // namespace UI
