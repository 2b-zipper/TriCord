#include "ui/disclaimer_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/discord_client.h"
#include "ui/image_manager.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include <3ds.h>

namespace UI {

const std::string DisclaimerScreen::DISCLAIMER_TEXT =
    "This project is developed for educational purposes only. "
    "This is an unofficial Discord client and is not affiliated with or "
    "endorsed by Discord Inc. "
    "Software is provided \"as is\", and you use it at your own risk. The "
    "developers assume no responsibility for any damages, data loss, or "
    "Discord ToS violations resulting from the use of this software.";

DisclaimerScreen::DisclaimerScreen() {}

void DisclaimerScreen::onEnter() {}

void DisclaimerScreen::update() {
  u32 kDown = hidKeysDown();
  if (kDown & KEY_A) {
    Config::getInstance().setDisclaimerAccepted(true);
    if (Discord::DiscordClient::getInstance().getState() ==
        Discord::ConnectionState::READY) {
      ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
    } else {
      ScreenManager::getInstance().setScreen(ScreenType::LOGIN);
    }
  } else if (kDown & KEY_B) {
    ScreenManager::getInstance().requestAppExit();
  }
}

void DisclaimerScreen::renderTop(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackground());

  float centerX = 200.0f;

  drawCenteredRichText(25.0f, 0.5f, 0.75f, 0.75f, ScreenManager::colorWhite(),
                       "Disclaimer", 400.0f);

  float lineW = 80.0f;
  C2D_DrawRectSolid(centerX - lineW / 2, 48.0f, 0.5f, lineW, 2.0f,
                    ScreenManager::colorPrimary());

  std::vector<std::string> lines =
      MessageUtils::wrapText(DISCLAIMER_TEXT, 360.0f, 0.45f);
  float y = 65.0f;
  for (const auto &line : lines) {
    drawCenteredRichText(y, 0.5f, 0.45f, 0.45f, ScreenManager::colorWhite(),
                         line, 400.0f);
    y += 16.0f;
  }
}

void DisclaimerScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_SceneBegin(target);
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());

  float centerY = 120.0f;

  drawCenteredText(centerY - 40.0f, 0.5f, 0.5f, 0.5f,
                   ScreenManager::colorWhite(), "Do you agree to these terms?",
                   320.0f);

  drawCenteredText(centerY + 10.0f, 0.5f, 0.5f, 0.5f,
                   ScreenManager::colorSuccess(), "A: Agree & Continue",
                   320.0f);

  drawCenteredText(centerY + 40.0f, 0.5f, 0.5f, 0.5f,
                   ScreenManager::colorError(), "B: Reject & Exit", 320.0f);
}

} // namespace UI
