#ifndef LOGIN_SCREEN_H
#define LOGIN_SCREEN_H

#include "discord/discord_client.h"
#include "discord/remote_auth.h"
#include "ui/country_picker.h"
#include "ui/screen_manager.h"
#include <string>
#include <vector>

namespace UI {

class LoginScreen : public Screen {
  public:
	LoginScreen();
	~LoginScreen() override;

	void update() override;
	bool isAnimating() const override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;
	void onEnter() override;
	void onExit() override;

  private:
	enum class Prompt { NONE, MFA, PHONE_CODE, UNDELETE };

	std::string statusMessage;
	std::string qrCodeUrl;

	std::string email;
	std::string phone;
	std::string password;
	bool phoneLogin = false;
	CountryPicker countryPicker;

	Prompt prompt = Prompt::NONE;
	std::string promptCode;
	std::string promptStatus;
	std::string mfaTicket;
	std::string mfaLoginInstanceId;
	std::string mfaMethod;
	std::string mfaPhone;
	bool mfaSmsAvailable = false;

	std::vector<uint8_t> qrCodeData;
	int qrCodeSize;
	bool qrCodeGenerated;

	float loadingAngle;
	float animTimer;
	bool ignoreInitialConnection = false;

	void drawLoadingSpinner(float x, float y, float radius);
	void checkTokenFile();
	void startQRLogin();
	void generateQRCode(const std::string &data);
	void drawQRCode(float x, float y, float size);

	bool offersSms() const;
	void setLoginStatus(const std::string &message);
	std::string loginIdentifier() const;
	void submitLogin(bool undelete);
	void handleLoginResult(const Discord::LoginResult &result);
	void requestMfaSms();
	void submitPrompt();
	void updatePrompt(const touchPosition &touch);
	void renderPrompt();

	void onStateChange(Discord::RemoteAuthState state, const std::string &info);
	void onUserScanned(const Discord::RemoteAuthUser &user);
	void onTokenReceived(const std::string &token);
	void onLoginSuccess(const std::string &token);
};

} // namespace UI

#endif // LOGIN_SCREEN_H
