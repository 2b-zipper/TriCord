#include "ui/login_screen.h"
#include "config.h"
#include "core/i18n.h"
#include "discord/discord_client.h"
#include "log.h"
#include "qrcodegen.h"
#include "ui/image_manager.h"
#include <3ds.h>
#include <cmath>

namespace UI {

LoginScreen::LoginScreen()
    : qrCodeSize(0), qrCodeGenerated(false), loadingAngle(0.0f),
      animTimer(0.0f) {
  statusMessage = Core::I18n::getInstance().get("login.status.initializing");
}

LoginScreen::~LoginScreen() {}

void LoginScreen::onEnter() {
  Logger::log("[LoginScreen] Entered");

  Discord::RemoteAuth::getInstance().setOnStateChange(
      [this](Discord::RemoteAuthState state, const std::string &info) {
        onStateChange(state, info);
      });

  Discord::RemoteAuth::getInstance().setOnUserScanned(
      [this](const Discord::RemoteAuthUser &user) { onUserScanned(user); });

  Discord::RemoteAuth::getInstance().setOnTokenReceived(
      [this](const std::string &token) { onTokenReceived(token); });

  bool hasToken = !Config::getInstance().getToken().empty();
  auto type = ScreenManager::getInstance().getCurrentType();

  bool isAlreadyConnected = Discord::DiscordClient::getInstance().getState() ==
                            Discord::ConnectionState::READY;

  if (type == ScreenType::ADD_ACCOUNT) {
    startQRLogin();
  } else if (!hasToken && !isAlreadyConnected) {
    startQRLogin();
  }

  if (type == ScreenType::ADD_ACCOUNT &&
      Discord::DiscordClient::getInstance().getState() ==
          Discord::ConnectionState::READY) {
    ignoreInitialConnection = true;
  } else {
    ignoreInitialConnection = false;
  }
}

void LoginScreen::onExit() {
  Logger::log("[LoginScreen] Exited");
  Discord::RemoteAuth::getInstance().cancel();
}

void LoginScreen::update() {
  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

  if (client.getState() == Discord::ConnectionState::READY) {
    if (!ignoreInitialConnection) {
      ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
      return;
    }
  } else if (client.getState() != Discord::ConnectionState::DISCONNECTED) {
    ignoreInitialConnection = false;
  }

  auto state = client.getState();
  bool isLoading = (state == Discord::ConnectionState::CONNECTING ||
                    state == Discord::ConnectionState::AUTHENTICATING);

  if (isLoading) {
    animTimer += 1.0f / 60.0f;
    if (animTimer >= 1.5f) {
      animTimer = 0.0f;
    }

    float t = 0.0f;
    if (animTimer < 1.0f) {

      float x = animTimer;
      const float c1 = 1.0f;
      const float c3 = c1 + 1.0f;

      float xm1 = x - 1.0f;
      t = 1.0f + c3 * pow(xm1, 3.0f) + c1 * pow(xm1, 2.0f);
    } else {

      t = 1.0f;
    }

    loadingAngle = 360.0f * t;
  }

  bool shouldAutoConnect = !Config::getInstance().getToken().empty() &&
                           (Discord::RemoteAuth::getInstance().getState() !=
                                Discord::RemoteAuthState::CONNECTING &&
                            Discord::RemoteAuth::getInstance().getState() !=
                                Discord::RemoteAuthState::WAITING_FOR_SCAN &&
                            Discord::RemoteAuth::getInstance().getState() !=
                                Discord::RemoteAuthState::WAITING_FOR_CONFIRM);
  auto screenType = ScreenManager::getInstance().getCurrentType();

  if (screenType == ScreenType::ADD_ACCOUNT) {
    shouldAutoConnect = false;
  }

  if ((screenType == ScreenType::LOGIN ||
       screenType == ScreenType::ADD_ACCOUNT) &&
      shouldAutoConnect) {
    if (client.getState() == Discord::ConnectionState::DISCONNECTED) {
      client.connect(Config::getInstance().getToken());
    }

    statusMessage = Discord::DiscordClient::getInstance().getStatusMessage();

    if (screenType == ScreenType::LOGIN)
      return;
  }

  u32 kDown = hidKeysDown();

  Discord::RemoteAuth::getInstance().poll();

  if ((kDown & KEY_B) && ScreenManager::getInstance().getCurrentType() ==
                             ScreenType::ADD_ACCOUNT) {
    if (Discord::DiscordClient::getInstance().getState() ==
        Discord::ConnectionState::READY) {
      ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
      return;
    }
  }

  if (kDown & KEY_TOUCH) {
    touchPosition touch;
    hidTouchRead(&touch);

    if (showMFAInput) {

      float dialogW = 260.0f;
      float dialogH = 140.0f;
      float dX = (BOTTOM_SCREEN_WIDTH - dialogW) / 2.0f;
      float dY = (BOTTOM_SCREEN_HEIGHT - dialogH) / 2.0f;

      float inputY = dY + 50.0f;
      float btnY = dY + 90.0f;

      if (touch.px >= dX + 20 && touch.px <= dX + dialogW - 20 &&
          touch.py >= inputY && touch.py <= inputY + 30) {
        SwkbdState swkbd;
        swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 1, 8);
        swkbdSetHintText(&swkbd, TR("login.swkbd.mfa").c_str());
        char buf[16];
        swkbdInputText(&swkbd, buf, sizeof(buf));
        mfaCode = buf;
      }

      if (touch.px >= dX + 20 && touch.px <= dX + dialogW - 20 &&
          touch.py >= btnY && touch.py <= btnY + 30) {
        if (!mfaCode.empty()) {
          statusMessage = TR("login.mfa.verifying");
          client.submitMFA(
              mfaTicket, mfaCode,
              [this](bool success, const std::string &token, bool mfa,
                     const std::string &ticket, const std::string &error) {
                if (success) {
                  onLoginSuccess(token);
                } else {
                  statusMessage = TR("login.status.failed") + ": " + error;
                }
              });
        }
      }
    } else {

      float fieldW = 280.0f;
      float fieldX = (BOTTOM_SCREEN_WIDTH - fieldW) / 2.0f;
      float emailY = 50.0f;
      float passY = emailY + 50.0f;
      float btnY = passY + 50.0f;

      if (touch.px >= fieldX && touch.px <= fieldX + fieldW &&
          touch.py >= emailY && touch.py <= emailY + 30) {
        SwkbdState swkbd;
        swkbdInit(&swkbd, SWKBD_TYPE_QWERTY, 2, -1);
        swkbdSetHintText(&swkbd, TR("login.swkbd.email").c_str());
        swkbdSetInitialText(&swkbd, email.c_str());
        swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
        char buf[256];
        if (swkbdInputText(&swkbd, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
          email = buf;
        }
      }

      if (touch.px >= fieldX && touch.px <= fieldX + fieldW &&
          touch.py >= passY && touch.py <= passY + 30) {
        SwkbdState swkbd;
        swkbdInit(&swkbd, SWKBD_TYPE_QWERTY, 2, -1);
        swkbdSetHintText(&swkbd, TR("login.swkbd.password").c_str());
        swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
        swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
        char buf[256];
        if (swkbdInputText(&swkbd, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
          password = buf;
        }
      }

      if (touch.px >= fieldX && touch.px <= fieldX + fieldW &&
          touch.py >= btnY && touch.py <= btnY + 35) {
        if (!email.empty() && !password.empty()) {
          statusMessage = TR("login.status.logging_in");
          client.performLogin(
              email, password,
              [this](bool success, const std::string &token, bool mfaRequired,
                     const std::string &ticket, const std::string &error) {
                if (success) {
                  onLoginSuccess(token);
                } else if (mfaRequired) {
                  statusMessage = TR("login.mfa.required");
                  mfaTicket = ticket;
                  showMFAInput = true;
                  mfaCode = "";
                } else {
                  statusMessage = TR("login.status.failed") + ": " + error;
                }
              });
        } else {
          statusMessage = TR("login.error.empty");
        }
      }
    }
  }
}

void LoginScreen::drawLoadingSpinner(float centerX, float centerY,
                                     float radius) {
  const int segments = 12;

  for (int i = 0; i < segments; i++) {
    float angle = (loadingAngle + i * (360.0f / segments)) * M_PI / 180.0f;
    float alpha = 1.0f - (float)i / segments;

    float x = centerX + cos(angle) * radius;
    float y = centerY + sin(angle) * radius;

    u8 a = (u8)(alpha * 255);
    C2D_DrawCircleSolid(x, y, 0.0f, 4.0f, C2D_Color32(0x5A, 0x65, 0xEA, a));
  }
}

void LoginScreen::renderTop(C3D_RenderTarget *target) {
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());
  C2D_SceneBegin(target);

  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  auto state = client.getState();
  bool isLoading = (state == Discord::ConnectionState::CONNECTING ||
                    state == Discord::ConnectionState::AUTHENTICATING);

  float centerX = TOP_SCREEN_WIDTH / 2.0f;
  float centerY = TOP_SCREEN_HEIGHT / 2.0f;

  if (isLoading) {
    C3D_Tex *discordTex = UI::ImageManager::getInstance().getLocalImage(
        "romfs:/discord.png", true);
    if (discordTex) {
      UI::ImageManager::ImageInfo info =
          UI::ImageManager::getInstance().getImageInfo("romfs:/discord.png");
      Tex3DS_SubTexture subtex;
      subtex.width = (u16)info.originalW;
      subtex.height = (u16)info.originalH;
      subtex.left = 0.0f;
      subtex.top = 0.0f;
      subtex.right = (float)info.originalW / discordTex->width;
      subtex.bottom = (float)info.originalH / discordTex->height;
      C2D_Image img = {discordTex, &subtex};
      float maxSpinnerSize = 90.0f;
      float scale = 1.0f;
      if (info.originalW > 0 && info.originalH > 0) {
        float scaleW = maxSpinnerSize / (float)info.originalW;
        float scaleH = maxSpinnerSize / (float)info.originalH;
        scale = (scaleW < scaleH) ? scaleW : scaleH;
      }
      float rad = (loadingAngle - 90.0f) * M_PI / 180.0f;
      C2D_DrawImageAtRotated(img, centerX, centerY, 0.6f, rad, nullptr, scale,
                             scale);
    } else {
      drawLoadingSpinner(centerX, centerY, 20.0f);
    }

    std::string status =
        Discord::DiscordClient::getInstance().getStatusMessage();
    if (status.empty())
      status = Core::I18n::getInstance().get("login.status.authenticating");
    drawCenteredText(centerY + 60.0f, 0.5f, 0.5f, 0.5f,
                     ScreenManager::colorText(), status, TOP_SCREEN_WIDTH);
    return;
  }

  float layoutY = 40.0f;

  float leftX = 30.0f;

  drawText(leftX, layoutY + 45.0f, 0.3f, 0.7f, 0.7f, ScreenManager::colorText(),
           Core::I18n::getInstance().get("login.welcome"));

  drawRichText(leftX, layoutY + 75.0f, 0.3f, 0.45f, 0.45f,
               ScreenManager::colorTextMuted(),
               Core::I18n::getInstance().get("login.excited"));

  float qrSize = 110.0f;
  float qrX = 285.0f;
  float qrY = 45.0f;

  float qrDrawX = qrX - (qrSize / 2.0f);

  if (qrCodeGenerated) {
    C2D_DrawRectSolid(qrDrawX - 8, qrY - 8, 0.25f, qrSize + 16, qrSize + 16,
                      ScreenManager::colorWhite());
    drawQRCode(qrDrawX, qrY, qrSize);
  } else {
    C2D_DrawRectSolid(qrDrawX - 8, qrY - 8, 0.25f, qrSize + 16, qrSize + 16,
                      ScreenManager::colorInput());

    std::string s = TR("login.generating");
    drawText(qrX - (measureText(s, 0.45f, 0.45f) / 2.0f),
             qrY + (qrSize / 2.0f) - 6.0f, 0.3f, 0.45f, 0.45f,
             ScreenManager::colorTextMuted(), s);
  }

  float textY = qrY + qrSize + 18.0f;

  std::string t1 = Core::I18n::getInstance().get("login.qr_title");
  drawText(qrX - (measureText(t1, 0.5f, 0.5f) / 2.0f), textY, 0.3f, 0.5f, 0.5f,
           ScreenManager::colorText(), t1);

  std::string t2 = Core::I18n::getInstance().get("login.qr_subtitle");
  drawText(qrX - (measureText(t2, 0.38f, 0.38f) / 2.0f), textY + 18.0f, 0.3f,
           0.38f, 0.38f, ScreenManager::colorTextMuted(), t2);
}

void LoginScreen::renderBottom(C3D_RenderTarget *target) {
  C2D_TargetClear(target, ScreenManager::colorBackgroundDark());
  C2D_SceneBegin(target);

  Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
  auto state = client.getState();
  bool isLoading = (state == Discord::ConnectionState::CONNECTING ||
                    state == Discord::ConnectionState::AUTHENTICATING);

  if (!isLoading) {

    if (showMFAInput) {

      float dialogW = 260.0f;
      float dialogH = 140.0f;
      float dX = (BOTTOM_SCREEN_WIDTH - dialogW) / 2.0f;
      float dY = (BOTTOM_SCREEN_HEIGHT - dialogH) / 2.0f;

      C2D_DrawRectSolid(0, 0, 0.4f, BOTTOM_SCREEN_WIDTH, BOTTOM_SCREEN_HEIGHT,
                        ScreenManager::colorOverlay());

      C2D_DrawRectSolid(dX, dY, 0.5f, dialogW, dialogH,
                        ScreenManager::colorInput());

      drawCenteredText(dY + 10.0f, 0.6f, 0.5f, 0.5f, ScreenManager::colorText(),
                       TR("login.mfa.title"), BOTTOM_SCREEN_WIDTH);
      drawCenteredText(dY + 30.0f, 0.6f, 0.4f, 0.4f,
                       ScreenManager::colorTextMuted(), TR("login.mfa.desc"),
                       BOTTOM_SCREEN_WIDTH);

      float inputY = dY + 50.0f;
      C2D_DrawRectSolid(dX + 20, inputY, 0.6f, dialogW - 40, 30.0f,
                        ScreenManager::colorBackgroundLight());
      std::string codeDisplay =
          mfaCode.empty() ? TR("login.mfa.hint") : mfaCode;
      drawText(dX + 25, inputY + 7.5f, 0.7f, 0.45f, 0.45f,
               ScreenManager::colorText(), codeDisplay);

      float btnY = dY + 90.0f;
      C2D_DrawRectSolid(dX + 20, btnY, 0.6f, dialogW - 40, 30.0f,
                        ScreenManager::colorSelection());
      drawCenteredText(btnY + 7.5f, 0.7f, 0.5f, 0.5f,
                       ScreenManager::colorWhite(), TR("login.mfa.verify"),
                       BOTTOM_SCREEN_WIDTH);

      drawCenteredText(dY + dialogH + 10.0f, 0.5f, 0.4f, 0.4f,
                       ScreenManager::colorTextMuted(), TR("login.mfa.cancel"),
                       BOTTOM_SCREEN_WIDTH);

    } else {

      float fieldW = 280.0f;
      float fieldX = (BOTTOM_SCREEN_WIDTH - fieldW) / 2.0f;
      float emailY = 50.0f;

      C2D_DrawRectSolid(fieldX, emailY, 0.5f, fieldW, 30.0f,
                        ScreenManager::colorInput());
      drawText(fieldX, emailY - 15.0f, 0.5f, 0.4f, 0.4f,
               ScreenManager::colorTextMuted(), TR("login.field.email"));
      std::string emailDisplay =
          email.empty() ? TR("login.field.email_hint") : email;
      u32 emailColor = email.empty() ? ScreenManager::colorTextMuted()
                                     : ScreenManager::colorText();
      drawText(fieldX + 5.0f, emailY + 7.5f, 0.5f, 0.45f, 0.45f, emailColor,
               emailDisplay);

      float passY = emailY + 50.0f;
      C2D_DrawRectSolid(fieldX, passY, 0.5f, fieldW, 30.0f,
                        ScreenManager::colorInput());
      drawText(fieldX, passY - 15.0f, 0.5f, 0.4f, 0.4f,
               ScreenManager::colorTextMuted(), TR("login.field.password"));
      std::string passDisplay = password.empty()
                                    ? TR("login.field.password_hint")
                                    : std::string(password.length(), '*');
      u32 passColor = password.empty() ? ScreenManager::colorTextMuted()
                                       : ScreenManager::colorText();
      drawText(fieldX + 5.0f, passY + 7.5f, 0.5f, 0.45f, 0.45f, passColor,
               passDisplay);

      float btnY = passY + 50.0f;
      C2D_DrawRectSolid(fieldX, btnY, 0.5f, fieldW, 35.0f,
                        ScreenManager::colorSelection());
      drawCenteredText(btnY + 9.5f, 0.5f, 0.5f, 0.5f,
                       ScreenManager::colorWhite(), TR("login.button.login"),
                       BOTTOM_SCREEN_WIDTH);

      drawCenteredText(BOTTOM_SCREEN_HEIGHT - 35.0f, 0.5f, 0.4f, 0.4f,
                       ScreenManager::colorError(), statusMessage,
                       BOTTOM_SCREEN_WIDTH);
    }
  }
}

void LoginScreen::startQRLogin() {
  Logger::log("[LoginScreen] Starting QR login");
  statusMessage = Core::I18n::getInstance().get("login.status.generating_qr");
  qrCodeGenerated = false;
  qrCodeUrl = "";

  bool started = Discord::RemoteAuth::getInstance().start();
  if (!started) {
    statusMessage = Core::I18n::getInstance().get("login.status.failed_auth");
  }
}

void LoginScreen::onStateChange(Discord::RemoteAuthState state,
                                const std::string &info) {
  statusMessage = info;

  if (state == Discord::RemoteAuthState::FAILED) {
    statusMessage =
        Core::I18n::getInstance().get("login.status.failed") + ": " + info;
  } else if (state == Discord::RemoteAuthState::CANCELLED) {
    statusMessage = Core::I18n::getInstance().get("common.cancel");
  }

  std::string url = Discord::RemoteAuth::getInstance().getQRCodeUrl();
  if (!url.empty() && url != qrCodeUrl) {
    generateQRCode(url);
  }
}

void LoginScreen::onUserScanned(const Discord::RemoteAuthUser &user) {
  statusMessage = Core::I18n::getInstance().get("login.status.scan_complete");
}

void LoginScreen::onTokenReceived(const std::string &ticket) {
  Logger::log("[LoginScreen] Ticket received: %s", ticket.c_str());
  statusMessage = Core::I18n::getInstance().get("login.status.exchanging");

  Discord::DiscordClient::getInstance().exchangeTicketForToken(
      ticket, [this](const std::string &encryptedToken) {
        if (encryptedToken.empty()) {
          statusMessage =
              Core::I18n::getInstance().get("login.status.failed_exchange");
          qrCodeGenerated = false;
          return;
        }

        Logger::log("[LoginScreen] Encrypted token received: %s",
                    encryptedToken.substr(0, 20).c_str());

        std::string token =
            Discord::RemoteAuth::getInstance().decryptToken(encryptedToken);
        if (token.empty()) {
          statusMessage =
              Core::I18n::getInstance().get("login.status.failed_decrypt");
          qrCodeGenerated = false;
          return;
        }

        Logger::log("[LoginScreen] Token decrypted: %s",
                    token.substr(0, 20).c_str());

        onLoginSuccess(token);
      });
}

void LoginScreen::onLoginSuccess(const std::string &token) {
  Config::getInstance().addAccount(
      Core::I18n::getInstance().get("login.account_name"), token);

  auto &client = Discord::DiscordClient::getInstance();
  client.logout();

  ScreenManager::getInstance().clearCaches();
  ScreenManager::getInstance().resetSelection();

  client.connect(token);
  statusMessage = TR("common.loading");

  ignoreInitialConnection = false;

  qrCodeGenerated = false;
}

void LoginScreen::generateQRCode(const std::string &data) {
  Logger::log("[LoginScreen] Generating QR code for: %s", data.c_str());

  Logger::log("[LoginScreen] Encoding QR code...");

  uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
  uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];

  bool ok = qrcodegen_encodeText(
      data.c_str(), tempBuffer, qrcode, qrcodegen_Ecc_MEDIUM,
      qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);

  if (!ok) {
    Logger::log("[LoginScreen] Failed to generate QR code");
    qrCodeGenerated = false;
    return;
  }

  qrCodeSize = qrcodegen_getSize(qrcode);
  Logger::log("[LoginScreen] QR code generated: %d x %d", qrCodeSize,
              qrCodeSize);

  qrCodeData.resize(qrCodeSize * qrCodeSize);
  for (int y = 0; y < qrCodeSize; y++) {
    for (int x = 0; x < qrCodeSize; x++) {
      qrCodeData[y * qrCodeSize + x] =
          qrcodegen_getModule(qrcode, x, y) ? 1 : 0;
    }
  }

  qrCodeGenerated = true;
  qrCodeUrl = data;
}

void LoginScreen::drawQRCode(float x, float y, float size) {
  if (!qrCodeGenerated || qrCodeSize == 0)
    return;

  float moduleSize = size / qrCodeSize;

  for (int row = 0; row < qrCodeSize; row++) {
    for (int col = 0; col < qrCodeSize; col++) {
      if (qrCodeData[row * qrCodeSize + col]) {

        C2D_DrawRectSolid(x + col * moduleSize, y + row * moduleSize, 0.3f,
                          moduleSize, moduleSize, C2D_Color32(0, 0, 0, 255));
      }
    }
  }
}

} // namespace UI
