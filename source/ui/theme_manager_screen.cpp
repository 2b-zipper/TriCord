#include "ui/theme_manager_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "network/http_client.h"
#include "utils/file_utils.h"
#include "utils/message_utils.h"
#include <citro2d.h>
#include <rapidjson/document.h>

namespace UI {

ThemeManagerScreen::ThemeManagerScreen() : selectedIndex(0), scrollOffset(0) {}

ThemeManagerScreen::~ThemeManagerScreen() {
	if (downloadThread.joinable()) {
		downloadThread.join();
	}
}

void ThemeManagerScreen::onEnter() { refreshThemes(); }

void ThemeManagerScreen::onExit() {}

void ThemeManagerScreen::refreshThemes() {
	themes.clear();
	std::vector<std::string> filenames = Config::getInstance().getAvailableThemes();

	for (const auto &fname : filenames) {
		ThemeItem item;
		item.filename = fname;
		item.displayName = fname;
		item.author = "";
		item.description = "";
		item.updateUrl = "";

		std::string path = std::string(CONFIG_DIR_PATH) + "/themes/" + fname + ".json";
		std::vector<char> buffer = Utils::File::readFile(path);
		if (!buffer.empty()) {
			rapidjson::Document doc;
			doc.Parse(buffer.data());
			if (!doc.HasParseError() && doc.IsObject()) {
				if (doc.HasMember("name") && doc["name"].IsString()) {
					item.displayName = doc["name"].GetString();
				}
				if (doc.HasMember("author") && doc["author"].IsString()) {
					item.author = doc["author"].GetString();
				}
				if (doc.HasMember("description") && doc["description"].IsString()) {
					item.description = doc["description"].GetString();
				}
				if (doc.HasMember("update_url") && doc["update_url"].IsString()) {
					item.updateUrl = doc["update_url"].GetString();
				}
			}
		}
		themes.push_back(item);
	}
}

void ThemeManagerScreen::update() {
	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();
	circlePosition pos;
	hidCircleRead(&pos);
	auto &config = Config::getInstance();

	if (deleteState == DeleteState::CONFIRMING) {
		if (kDown & KEY_A) {
			if (themeToDeleteIndex > 0 && themeToDeleteIndex - 1 < (int)themes.size()) {
				std::string themeFile = themes[themeToDeleteIndex - 1].filename;
				config.deleteTheme(themeFile);
				refreshThemes();

				if (selectedIndex >= (int)themes.size()) {
					selectedIndex = (int)themes.size() - 1;
				}
				if (selectedIndex < 0) {
					selectedIndex = 0;
				}

				if (scrollOffset > selectedIndex) {
					scrollOffset = (float)selectedIndex;
				}
				if (scrollOffset + 4 < selectedIndex) {
					scrollOffset = (float)(selectedIndex - 4);
				}
			}
			deleteState = DeleteState::NONE;
		} else if (kDown & KEY_B) {
			deleteState = DeleteState::NONE;
		}
		return;
	}

	if (kDown & KEY_B) {
		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}

	int totalItems = themes.size();
	if (totalItems == 0) {
		return;
	}

	u32 moveDir = 0;
	if (kDown & (KEY_UP | KEY_DOWN)) {
		repeatTimer = 30;
		lastKey = (kDown & KEY_UP) ? KEY_UP : KEY_DOWN;
		moveDir = lastKey;
	}

	if (pos.dy > 40 && lastKey != KEY_UP) {
		moveDir = KEY_UP;
		repeatTimer = 30;
		lastKey = KEY_UP;
	} else if (pos.dy < -40 && lastKey != KEY_DOWN) {
		moveDir = KEY_DOWN;
		repeatTimer = 30;
		lastKey = KEY_DOWN;
	}

	if (lastKey != 0 && (kHeld & (KEY_UP | KEY_DOWN) || abs(pos.dy) > 40)) {
		if (--repeatTimer <= 0) {
			moveDir = lastKey;
			repeatTimer = 6;
		}
	} else {
		lastKey = 0;
	}

	if (moveDir & KEY_UP) {
		selectedIndex--;
		if (selectedIndex < 0) {
			selectedIndex = totalItems - 1;
		}
		if (selectedIndex < (int)scrollOffset) {
			scrollOffset = (float)selectedIndex;
		} else if (selectedIndex >= (int)scrollOffset + 5) {
			scrollOffset = (float)(selectedIndex - 4);
		}
		if (!isUpdating) {
			updateStatus = "";
		}
	}

	if (moveDir & KEY_DOWN) {
		selectedIndex++;
		if (selectedIndex >= totalItems) {
			selectedIndex = 0;
		}
		if (selectedIndex >= (int)scrollOffset + 5) {
			scrollOffset = (float)(selectedIndex - 4);
		} else if (selectedIndex < (int)scrollOffset) {
			scrollOffset = (float)selectedIndex;
		}
		if (!isUpdating) {
			updateStatus = "";
		}
	}

	if (kDown & KEY_A) {
		std::string themeFile = themes[selectedIndex].filename;
		if (config.getSelectedThemeName() == themeFile && config.isCustomThemeEnabled()) {
			config.setCustomThemeEnabled(false);
		} else {
			config.loadThemeFromFile(themeFile);
			config.setCustomThemeEnabled(true);
		}
	}

	if (kDown & KEY_X) {
		themeToDeleteIndex = selectedIndex + 1;
		deleteState = DeleteState::CONFIRMING;
	}

	if (kDown & KEY_Y) {
		const auto &item = themes[selectedIndex];
		if (!item.updateUrl.empty() && !isUpdating) {
			if (downloadThread.joinable()) {
				downloadThread.join();
			}

			isUpdating = true;
			updateStatus = TR("theme.status.updating");
			ScreenManager::getInstance().showToast(updateStatus);
			downloadThread = std::thread(&ThemeManagerScreen::downloadTheme, this, item.updateUrl, item.filename);
		}
	}

	if (!isUpdating && shouldRefresh) {
		if (downloadThread.joinable()) {
			downloadThread.join();
		}
		shouldRefresh = false;
		refreshThemes();
		if (!themeToReload.empty()) {
			config.loadThemeFromFile(themeToReload);
			themeToReload = "";
		}
		if (!updateStatus.empty()) {
			ScreenManager::getInstance().showToast(updateStatus);
		}
	}
}

void ThemeManagerScreen::downloadTheme(const std::string &url, const std::string &filename) {
	updateStatus = TR("theme.status.updating");

	Network::HttpClient http;
	auto resp = http.get(url);

	if (resp.success) {
		std::string path = std::string(CONFIG_DIR_PATH) + "/themes/" + filename + ".json";
		if (Utils::File::writeFile(path, resp.body)) {
			updateStatus = TR("theme.status.success");
			shouldRefresh = true;

			if (Config::getInstance().getSelectedThemeName() == filename &&
			    Config::getInstance().isCustomThemeEnabled()) {
				themeToReload = filename;
			}
		} else {
			updateStatus = TR("theme.status.failed_save");
			shouldRefresh = true;
		}
	} else {
		updateStatus = Core::I18n::format(TR("theme.status.failed_code"), std::to_string(resp.statusCode));
		if (resp.statusCode == 0) {
			updateStatus += " (" + resp.error + ")";
		}
		shouldRefresh = true;
	}
	isUpdating = false;
}

void ThemeManagerScreen::renderTop(C3D_RenderTarget *target) {
	const auto &theme = Config::getInstance().getTheme();
	C2D_TargetClear(target, theme.bg);
	C2D_SceneBegin(target);

	float headerH = 26.0f;
	float topScreenWidth = 400.0f;

	C2D_DrawRectSolid(0, 0, 0.9f, topScreenWidth, headerH, ScreenManager::colorHeaderGlass());
	C2D_DrawRectSolid(0, headerH - 1.0f, 0.91f, topScreenWidth, 1.0f, ScreenManager::colorHeaderBorder());

	std::string titleText = TR("theme.manager.title");
	drawCenteredText(4.0f, 0.95f, 0.52f, 0.52f, theme.text, titleText, topScreenWidth);

	float x = 10, y = headerH + 6.0f;
	float width = topScreenWidth - 20.0f;
	float itemHeight = 36.0f;

	bool isEnabled = Config::getInstance().isCustomThemeEnabled();
	std::string currentSelected = Config::getInstance().getSelectedThemeName();

	int totalItems = themes.size();
	for (int i = (int)scrollOffset; i < totalItems; i++) {
		if (y > 240.0f) {
			break;
		}

		bool isSel = (selectedIndex == i);
		u32 itemBg = isSel ? theme.bg_light : theme.bg_dark;

		drawRoundedRect(x, y, 0.5f, width, itemHeight, 8.0f, itemBg);

		if (isSel) {
			drawRoundedRect(x, y + 4, 0.55f, 4, itemHeight - 8.0f, 2.0f, theme.selection);
		}

		u32 tc = isSel ? theme.text : theme.text_muted;

		std::string label = themes[i].displayName;
		bool isActive = (themes[i].filename == currentSelected) && isEnabled;

		drawText(x + 15, y + 9.0f, 0.6f, 0.45f, 0.45f, tc, label);

		float toggleW = 34.0f;
		float toggleH = 18.0f;
		float toggleX = topScreenWidth - 30.0f - toggleW;
		float toggleY = y + (itemHeight - toggleH) / 2.0f;
		u32 tBg = isActive ? theme.selection : theme.separator;

		drawRoundedRect(toggleX, toggleY, 0.6f, toggleW, toggleH, toggleH / 2.0f, tBg);
		float circSize = 14.0f;
		float circX = isActive ? (toggleX + toggleW - circSize - 2.0f) : (toggleX + 2.0f);
		drawCircle(circX + circSize / 2.0f, toggleY + circSize / 2.0f + 2.0f, 0.65f, circSize / 2.0f, theme.pure_white);

		y += itemHeight + 6.0f;
	}

	if (deleteState == DeleteState::CONFIRMING) {
		float cw = 280.0f;
		float ch = 110.0f;
		float cx = (topScreenWidth - cw) / 2.0f;
		float cy = (240.0f - ch) / 2.0f;

		drawOverlay(0.85f);
		drawPopupBackground(cx, cy, cw, ch, 0.9f, 12.0f);

		std::string themeName = themes[themeToDeleteIndex - 1].displayName;
		std::string title = Core::I18n::format(TR("theme.delete.title"), themeName);

		drawCenteredText(cy + 12.0f, 0.92f, 0.5f, 0.5f, theme.text, title, topScreenWidth);

		drawCenteredText(cy + 42.0f, 0.92f, 0.45f, 0.45f, theme.text_muted, TR("theme.delete.desc"), topScreenWidth);

		std::string help = "\uE000: " + TR("common.delete") + "  \uE001: " + TR("common.cancel");
		drawCenteredText(cy + ch - 22.0f, 0.92f, 0.4f, 0.4f, theme.error, help, topScreenWidth);
	}
}

void ThemeManagerScreen::renderBottom(C3D_RenderTarget *target) {
	const auto &theme = Config::getInstance().getTheme();
	C2D_TargetClear(target, theme.bg_dark);
	C2D_SceneBegin(target);

	float bottomScreenWidth = 320.0f;

	std::string label;
	std::string desc;
	std::string fileLabel = "";
	bool canUpdate = false;

	if (selectedIndex >= 0 && selectedIndex < (int)themes.size()) {
		const auto &item = themes[selectedIndex];
		label = item.displayName;
		if (!item.author.empty()) {
			label += " by " + item.author;
		}
		desc = item.description;
		fileLabel = "File: " + item.filename + ".json";
		canUpdate = !item.updateUrl.empty();
	}

	drawText(35.0f, 10.0f, 0.6f, 0.5f, 0.5f, theme.text, label);
	C2D_DrawRectSolid(10, 32, 0.5f, bottomScreenWidth - 20, 1, theme.separator);

	float currentY = 40.0f;
	if (!desc.empty()) {
		std::vector<std::string> lines = MessageUtils::wrapText(desc, bottomScreenWidth - 20, 0.45f);
		for (const auto &line : lines) {
			drawText(10.0f, currentY, 0.6f, 0.45f, 0.45f, theme.text_muted, line);
			currentY += 15.0f;
		}
		currentY += 5.0f;
	}

	if (!fileLabel.empty()) {
		drawText(10.0f, currentY, 0.6f, 0.4f, 0.4f, theme.text_muted, fileLabel);
		currentY += 18.0f;
	}

	float infoY = std::max(currentY + 10.0f, 135.0f);

	drawText(10.0f, infoY, 0.6f, 0.45f, 0.45f, theme.accent, "Tip:");
	drawText(10.0f, infoY + 15.0f, 0.6f, 0.45f, 0.45f, theme.text_muted, TR("theme.tip.desc"));
	drawText(10.0f, infoY + 30.0f, 0.6f, 0.45f, 0.45f, theme.text, "sdmc:/3ds/TriCord/themes/");

	float keysY = 240.0f - 25.0f;

	std::string help = "\uE000: " + TR("theme.help.select") + "  \uE001: " + TR("theme.help.back");
	if (!themes.empty()) {
		help += "  \uE002: " + TR("theme.help.delete");
		if (canUpdate) {
			help += "  \uE003: " + TR("theme.help.update");
		}
	}
	drawText(10.0f, keysY, 0.5f, 0.4f, 0.4f, theme.text_muted, help);
}

} // namespace UI
