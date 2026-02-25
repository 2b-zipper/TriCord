#include "ui/about_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include <3ds.h>
#include <cmath>

namespace UI {

AboutScreen::AboutScreen()
    : animTimer(0.0f), logoBounce(0.0f), scrollOffset(0.0f), maxScroll(0.0f) {}

void AboutScreen::onEnter() {
  animTimer = 0.0f;
  logoBounce = 0.0f;
  scrollOffset = 0.0f;
}

void AboutScreen::update() {
  animTimer += 0.02f;
  logoBounce = std::sin(animTimer) * 5.0f;

  u32 kDown = hidKeysDown();
  u32 kHeld = hidKeysHeld();

  if (kDown & (KEY_B | KEY_SELECT)) {
    ScreenManager::getInstance().returnToPreviousScreen();
  }

  float scrollSpeed = 2.0f;
  if (kHeld & KEY_DOWN)
    scrollOffset += scrollSpeed;
  if (kHeld & KEY_UP)
    scrollOffset -= scrollSpeed;

  touchPosition touch;
  hidTouchRead(&touch);
  static touchPosition lastTouch = {0, 0};
  static bool isTouching = false;

  if (kDown & KEY_TOUCH) {
    isTouching = true;
    lastTouch = touch;
  } else if (kHeld & KEY_TOUCH) {
    if (isTouching) {
      float dy = (float)lastTouch.py - touch.py;
      scrollOffset += dy;
      lastTouch = touch;
    }
  } else {
    isTouching = false;
  }

  circlePosition pos;
  hidCircleRead(&pos);
  if (std::abs(pos.dy) > 20) {
    scrollOffset -= (float)pos.dy / 10.0f;
  }

  if (scrollOffset < 0.0f)
    scrollOffset = 0.0f;
  if (scrollOffset > maxScroll)
    scrollOffset = maxScroll;
}

void AboutScreen::renderTop(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackground());

  float centerX = 200.0f;
  float centerY = 120.0f;

  drawCircle(350, 40, 0.1f, 60.0f, C2D_Color32(88, 101, 242, 40));
  drawCircle(50, 200, 0.1f, 80.0f, C2D_Color32(235, 69, 158, 30));

  C3D_Tex *logo =
      ImageManager::getInstance().getLocalImage("romfs:/discord.png", true);
  if (logo) {
    ImageManager::ImageInfo info =
        ImageManager::getInstance().getImageInfo("romfs:/discord.png");
    Tex3DS_SubTexture sub;
    sub.width = (u16)info.originalW;
    sub.height = (u16)info.originalH;
    sub.left = 0.0f;
    sub.top = 0.0f;
    sub.right = (float)info.originalW / logo->width;
    sub.bottom = (float)info.originalH / logo->height;
    C2D_Image img = {logo, &sub};

    float scale = 80.0f / info.originalW;
    C2D_DrawImageAtRotated(img, centerX, centerY - 20.0f + logoBounce, 0.5f,
                           -M_PI / 2.0f, nullptr, scale, scale);
  }

  drawCenteredRichText(centerY + 30.0f, 0.5f, 0.8f, 0.8f,
                       ScreenManager::colorWhite(), "TriCord", 400.0f);

  std::string verStr = "Version " + std::string(APP_VERSION);
  drawCenteredText(centerY + 55.0f, 0.5f, 0.5f, 0.5f,
                   ScreenManager::colorTextMuted(), verStr, 400.0f);

  float lineW = 100.0f;
  C2D_DrawRectSolid(centerX - lineW / 2, centerY + 25.0f, 0.5f, lineW, 2.0f,
                    ScreenManager::colorPrimary());
}

void AboutScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

  float x = 20.0f;
  float y = 40.0f - scrollOffset;

  auto drawSectionTitle = [&](const std::string &title, bool first = false) {
    if (!first)
      y += 12.0f;
    float scale = 0.42f;
    drawText(x, y, 0.5f, scale, scale, ScreenManager::colorText(), title);
    float w = measureText(title, scale, scale);
    y += 13.0f;
    C2D_DrawRectSolid(x, y - 2.0f, 0.5f, w, 1.0f,
                      ScreenManager::colorSeparator());
    y += 5.0f;
  };

  auto drawEntry = [&](const std::string &name, const std::string &desc) {
    drawText(x + 8.0f, y, 0.5f, 0.38f, 0.38f, ScreenManager::colorWhite(),
             name);
    float w = measureText(name, 0.38f, 0.38f);
    drawText(x + 12.0f + w, y + 1.0f, 0.5f, 0.35f, 0.35f,
             ScreenManager::colorTextMuted(), " - " + desc);
    y += 14.0f;
  };

  drawSectionTitle("Credits", true);
  drawEntry("2b-zipper", "Lead Developer");
  drawEntry("Str4ky", "French Translation");
  drawEntry("AverageJohtonian", "Spanish Translation");
  drawEntry("RossoDev", "Italian Translation");
  drawEntry("MorrisTheGamer", "German Translation");
  drawEntry("Discord Userdoccers", "API Research");

  y += 2.0f;
  drawText(x + 8.0f, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
           "And all other contributors!");
  y += 12.0f;

  drawSectionTitle("Built With");
  auto drawLib = [&](const std::string &name) {
    drawText(x + 8.0f, y, 0.5f, 0.36f, 0.36f, ScreenManager::colorWhite(),
             "\u2022 " + name);
    y += 13.0f;
  };
  drawLib("libctru, citro3d, citro2d");
  drawLib("libcurl, mbedtls, RapidJSON");
  drawLib("stb_image, qrcodegen, zlib");
  drawLib("Twemoji Assets");

  drawSectionTitle("Source Code");
  drawText(x + 8.0f, y, 0.5f, 0.36f, 0.36f, ScreenManager::colorTextMuted(),
           "Licensed under GPL v3.0");
  y += 13.0f;
  drawRichText(x + 8.0f, y, 0.5f, 0.36f, 0.36f, ScreenManager::colorLink(),
               "https://github.com/2b-zipper/TriCord");

  y += 25.0f;
  C2D_DrawRectSolid(x, y, 0.5f, 280.0f, 1.0f, ScreenManager::colorSeparator());
  y += 8.0f;

  auto drawLegalText = [&](const std::string &text, float scale = 0.32f) {
    auto lines = MessageUtils::wrapText(text, 280.0f, scale);
    for (const auto &line : lines) {
      drawText(x, y, 0.5f, scale, scale, ScreenManager::colorTextMuted(), line);
      y += (scale * 35.0f);
    }
  };

  drawLegalText("Disclaimer:", 0.36f);
  y += 2.0f;
  drawLegalText("This project is developed for educational purposes only. This "
                "is an unofficial Discord client and is not affiliated with or "
                "endorsed by Discord Inc.");
  y += 5.0f;
  drawLegalText("This software is provided \"as is\", and you use it at your "
                "own risk. The use of this application is entirely the user's "
                "own responsibility. The developers assume no responsibility "
                "for any damages, data loss, or violations of Discord's ToS.");
  y += 30.0f;

  maxScroll =
      std::max(0.0f, (y + scrollOffset) - (BOTTOM_SCREEN_HEIGHT - 35.0f));

  C2D_DrawRectSolid(0.0f, 0.0f, 0.8f, 320.0f, 35.0f,
                    ScreenManager::colorBackgroundDark());
  drawText(35.0f, 10.0f, 0.85f, 0.5f, 0.5f, ScreenManager::colorText(),
           "About TriCord");
  C2D_DrawRectSolid(10.0f, 32.0f, 0.85f, 300.0f, 1.0f,
                    ScreenManager::colorSeparator());

  drawCenteredText(
      240.0f - 22.0f, 0.9f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
      "\uE001: " + Core::I18n::getInstance().get("common.back"), 320.0f);
  if (maxScroll > 0) {
    float areaH = (240.0f - 35.0f - 30.0f) - 10.0f;
    float barX = 320.0f - 8.0f;
    float barY = 35.0f + 5.0f;
    C2D_DrawRectSolid(barX, barY, 0.85f, 4.0f, areaH,
                      C2D_Color32(255, 255, 255, 20));
    float thumbH = std::max(20.0f, areaH * (areaH / (areaH + maxScroll)));
    float thumbY = barY + (scrollOffset / maxScroll) * (areaH - thumbH);
    drawRoundedRect(barX, thumbY, 0.9f, 4.0f, thumbH, 2.0f,
                    ScreenManager::colorSelection());
  }
}

} // namespace UI
