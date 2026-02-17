#include "ui/about_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include <3ds.h>
#include <cmath>

namespace UI {

AboutScreen::AboutScreen() : animTimer(0.0f), logoBounce(0.0f) {}

void AboutScreen::onEnter() {
  animTimer = 0.0f;
  logoBounce = 0.0f;
}

void AboutScreen::update() {
  animTimer += 0.02f;
  logoBounce = std::sin(animTimer) * 5.0f;

  u32 kDown = hidKeysDown();
  if (kDown & (KEY_B | KEY_SELECT)) {
    ScreenManager::getInstance().returnToPreviousScreen();
  }
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

  drawText(45.0f, 10.0f, 0.6f, 0.5f, 0.5f, ScreenManager::colorText(),
           "About TriCord");

  C2D_DrawRectSolid(10.0f, 32.0f, 0.5f, 320.0f - 20.0f, 1.0f,
                    ScreenManager::colorSeparator());

  float x = 15.0f;
  float y = 44.0f;

  auto drawInfo = [&](const std::string &label, const std::string &val) {
    drawText(x, y, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), label);
    drawRichText(x + 70.0f, y, 0.5f, 0.4f, 0.4f, ScreenManager::colorWhite(),
                 val);
    y += 14.0f;
  };

  drawInfo("Developer:", "2b-zipper");
  drawInfo("Libraries:", "libctru, citro3d, citro2d, libcurl, mbedtls, zlib");
  drawRichText(x + 70.0f, y, 0.5f, 0.4f, 0.4f, ScreenManager::colorWhite(),
               "RapidJSON, stb_image, qrcodegen, Twemoji");
  y += 14.0f;
  drawInfo("License:", "GNU GPL v3.0");

  y += 8.0f;
  drawText(x, y, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
           "Special Thanks:");
  y += 14.0f;

  float subX = x + 10.0f;
  drawText(subX, y, 0.5f, 0.38f, 0.38f, ScreenManager::colorText(),
           "Discord Userdoccers (API Docs)");

  y += 18.0f;
  drawText(x, y, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
           "GitHub Repo:");
  y += 14.0f;
  drawRichText(x + 10.0f, y, 0.5f, 0.4f, 0.4f, ScreenManager::colorLink(),
               "https://github.com/2b-zipper/TriCord");

  y += 18.0f;
  drawText(x, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
           "This is an unofficial client not affiliated with Discord Inc.");

  drawCenteredText(
      240.0f - 25.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
      "\uE001: " + Core::I18n::getInstance().get("common.back"), 320.0f);
}

} // namespace UI
