#include "ui/settings_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "log.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include <3ds.h>

namespace UI {

SettingsScreen::SettingsScreen() : selectedIndex(0), scrollOffset(0.0f) {}

SettingsScreen::~SettingsScreen() {}

void SettingsScreen::onEnter() {
  Logger::log("[SettingsScreen] Entered");
  items.clear();

  SettingItem language;
  language.label = TR("settings.language");
  language.description = TR("settings.desc.language");
  language.type = SettingItemType::INTEGER;
  std::string langCode = Config::getInstance().getLanguage();
  language.value = (langCode == "ja") ? 1 : 0;
  language.min = 0;
  language.max = 1;
  language.valueFormatter = [](int val) {
    if (val == 0)
      return "English";
    if (val == 1)
      return "日本語";
    return "English";
  };
  language.onUpdate = [this](int val) {
    std::string newLang = (val == 1) ? "ja" : "en";
    Config::getInstance().setLanguage(newLang);
    Config::getInstance().saveSettings();
    ScreenManager::getInstance().getHamburgerMenu().refreshStrings();
    this->onEnter();
  };
  items.push_back(language);

  SettingItem timezone;
  timezone.label = TR("settings.timezone");
  timezone.description = TR("settings.desc.timezone");
  timezone.type = SettingItemType::INTEGER;
  timezone.value = Config::getInstance().getTimezoneOffset();
  timezone.min = -12;
  timezone.max = 14;
  timezone.valueFormatter = [](int val) {
    std::string s = (val >= 0 ? "+" : "") + std::to_string(val);
    if (val == 0)
      s += " (UTC/GMT)";
    else if (val == 1)
      s += " (CET)";
    else if (val == -5)
      s += " (EST)";
    else if (val == -6)
      s += " (CST)";
    else if (val == -8)
      s += " (PST)";
    else if (val == 5)
      s += " (IST)";
    else if (val == 9)
      s += " (JST/KST)";
    else if (val == 10)
      s += " (AEST)";
    return s;
  };
  timezone.onUpdate = [](int val) {
    Config::getInstance().setTimezoneOffset(val);
    Config::getInstance().saveSettings();
  };
  items.push_back(timezone);

  SettingItem theme;
  theme.label = TR("settings.theme");
  theme.description = TR("settings.desc.theme");
  theme.type = SettingItemType::INTEGER;
  theme.value = Config::getInstance().getThemeType();
  theme.min = 0;
  theme.max = 2;
  theme.valueFormatter = [](int val) {
    if (val == 0)
      return TR("settings.theme.dark");
    if (val == 1)
      return TR("settings.theme.light");
    return TR("settings.theme.custom");
  };
  theme.onUpdate = [](int val) {
    Config::getInstance().setThemeType(val);
    if (val == 2) {
      Config::getInstance().loadTheme();
    }
    Config::getInstance().saveSettings();
  };
  items.push_back(theme);

  SettingItem typing;
  typing.label = TR("settings.typing_indicator");
  typing.description = TR("settings.desc.typing_indicator");
  typing.type = SettingItemType::INTEGER;
  typing.value = Config::getInstance().isTypingIndicatorEnabled() ? 1 : 0;
  typing.min = 0;
  typing.max = 1;
  typing.valueFormatter = [](int val) {
    return (val == 1) ? TR("common.enabled") : TR("common.disabled");
  };
  typing.onUpdate = [](int val) {
    Config::getInstance().setTypingIndicatorEnabled(val == 1);
    Config::getInstance().saveSettings();
  };
  items.push_back(typing);

  SettingItem fileLogging;
  fileLogging.label = TR("settings.file_logging");
  fileLogging.description = TR("settings.desc.file_logging");
  fileLogging.type = SettingItemType::INTEGER;
  fileLogging.value = Config::getInstance().isFileLoggingEnabled() ? 1 : 0;
  fileLogging.min = 0;
  fileLogging.max = 1;
  fileLogging.valueFormatter = [](int val) {
    return (val == 1) ? TR("common.enabled") : TR("common.disabled");
  };
  fileLogging.onUpdate = [](int val) {
    Config::getInstance().setFileLoggingEnabled(val == 1);
  };
  items.push_back(fileLogging);
}

void SettingsScreen::onExit() { Logger::log("[SettingsScreen] Exited"); }

void SettingsScreen::update() {
  u32 kDown = hidKeysDown();

  if (items.empty())
    return;

  if (kDown & KEY_UP) {
    if (selectedIndex > 0)
      selectedIndex--;
  } else if (kDown & KEY_DOWN) {
    if (selectedIndex < (int)items.size() - 1)
      selectedIndex++;
  }

  SettingItem &selected = items[selectedIndex];
  if (selected.type == SettingItemType::INTEGER) {
    if (kDown & KEY_RIGHT) {
      if (selected.value < selected.max) {
        selected.value++;
        if (selected.onUpdate)
          selected.onUpdate(selected.value);
      }
    } else if (kDown & KEY_LEFT) {
      if (selected.value > selected.min) {
        selected.value--;
        if (selected.onUpdate)
          selected.onUpdate(selected.value);
      }
    }
  }

  if (kDown & KEY_B) {
    ScreenManager::getInstance().returnToPreviousScreen();
  }
}

void SettingsScreen::renderTop(C3D_RenderTarget *target) {
  C2D_TargetClear(target, ScreenManager::colorBackground());
  C2D_SceneBegin(target);

  float headerH = 26.0f;
  C2D_DrawRectSolid(0, 0, 0.9f, TOP_SCREEN_WIDTH, headerH,
                    ScreenManager::colorHeaderGlass());
  C2D_DrawRectSolid(0, headerH - 1.0f, 0.91f, TOP_SCREEN_WIDTH, 1.0f,
                    ScreenManager::colorHeaderBorder());

  drawCenteredText(4.0f, 0.95f, 0.52f, 0.52f, ScreenManager::colorText(),
                   TR("settings.title"), TOP_SCREEN_WIDTH);

  float y = headerH + 10.0f;
  for (int i = 0; i < (int)items.size(); i++) {
    const auto &item = items[i];
    bool isSelected = (i == selectedIndex);

    if (isSelected) {
      C2D_DrawRectSolid(10, y, 0.5f, TOP_SCREEN_WIDTH - 20, 34,
                        ScreenManager::colorBackgroundLight());
      C2D_DrawRectSolid(10, y, 0.55f, 4, 34, ScreenManager::colorSelection());
    } else {
      C2D_DrawRectSolid(10, y, 0.5f, TOP_SCREEN_WIDTH - 20, 34,
                        ScreenManager::colorBackgroundDark());
    }

    u32 textColor = isSelected ? ScreenManager::colorText()
                               : ScreenManager::colorTextMuted();
    drawText(25.0f, y + 9.0f, 0.6f, 0.45f, 0.45f, textColor, item.label);

    float centerX = TOP_SCREEN_WIDTH - 80.0f;
    std::string valStr = item.valueFormatter ? item.valueFormatter(item.value)
                                             : std::to_string(item.value);
    float valWidth = measureText(valStr, 0.45f, 0.45f);

    if (isSelected && item.type == SettingItemType::INTEGER) {
      u32 arrowColor = ScreenManager::colorSelection();
      if (item.value > item.min)
        drawText(centerX - 55.0f, y + 10.0f, 0.6f, 0.45f, 0.45f, arrowColor,
                 "<");
      if (item.value < item.max)
        drawText(centerX + 45.0f, y + 10.0f, 0.6f, 0.45f, 0.45f, arrowColor,
                 ">");
    }

    drawText(centerX - (valWidth / 2.0f), y + 10.0f, 0.6f, 0.45f, 0.45f,
             isSelected ? ScreenManager::colorText()
                        : ScreenManager::colorTextMuted(),
             valStr);

    y += 42.0f;
  }
}

void SettingsScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());
  C2D_SceneBegin(target);

  if (!items.empty() && selectedIndex >= 0 &&
      selectedIndex < (int)items.size()) {
    const auto &item = items[selectedIndex];

    drawText(45.0f, 10.0f, 0.6f, 0.5f, 0.5f, ScreenManager::colorText(),
             item.label);

    C2D_DrawRectSolid(10, 32, 0.5f, BOTTOM_SCREEN_WIDTH - 20, 1,
                      ScreenManager::colorSeparator());

    float descY = 40.0f;
    auto lines = MessageUtils::wrapText(item.description,
                                        BOTTOM_SCREEN_WIDTH - 20, 0.5f);
    for (const auto &line : lines) {
      drawText(10.0f, descY, 0.6f, 0.45f, 0.45f,
               ScreenManager::colorTextMuted(), line);
      descY += 15.0f;
    }
  }

  float keysY = BOTTOM_SCREEN_HEIGHT - 25.0f;

  drawText(10.0f, keysY, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
           "\uE079\uE07A: " + TR("common.navigate") + "  \uE07B\uE07C: " +
               TR("common.adjust") + "  \uE001: " + TR("common.back"));
}

void SettingsScreen::saveAndExit() {
  Config::getInstance().saveSettings();
  ScreenManager::getInstance().setSelectedGuildId("");
  ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
}

} // namespace UI
