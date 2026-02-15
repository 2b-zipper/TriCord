#ifndef LOGIN_SCREEN_H
#define LOGIN_SCREEN_H

#include "discord/remote_auth.h"
#include "ui/screen_manager.h"
#include <string>
#include <vector>

namespace UI {

class LoginScreen : public Screen {
public:
  LoginScreen();
  ~LoginScreen() override;

  void update() override;
  void renderTop(C3D_RenderTarget *target) override;
  void renderBottom(C3D_RenderTarget *target) override;
  void onEnter() override;
  void onExit() override;

private:
  std::string statusMessage;
  std::string qrCodeUrl;

  // Email Login
  std::string email;
  std::string password;
  std::string mfaTicket;
  bool isEmailFieldSelected = false;
  bool isPasswordFieldSelected = false;
  bool showMFAInput = false;
  std::string mfaCode;

  std::vector<uint8_t> qrCodeData;
  int qrCodeSize;
  bool qrCodeGenerated;

  float loadingAngle;
  float animTimer;
  bool ignoreInitialConnection = false;

  void drawLoadingSpinner(float x, float y, float radius);
  void startQRLogin();
  void generateQRCode(const std::string &data);
  void drawQRCode(float x, float y, float size);

  void onStateChange(Discord::RemoteAuthState state, const std::string &info);
  void onUserScanned(const Discord::RemoteAuthUser &user);
  void onTokenReceived(const std::string &token);
  void onLoginSuccess(const std::string &token);
};

} // namespace UI

#endif // LOGIN_SCREEN_H
