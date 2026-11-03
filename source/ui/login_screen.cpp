#include "ui/login_screen.h"
#include "config.h"
#include "core/i18n.h"
#include "discord/discord_client.h"
#include "log.h"
#include "qrcodegen.h"
#include "ui/image_manager.h"
#include "utils/file_utils.h"
#include "utils/redraw.h"
#include "utils/string_utils.h"
#include <3ds.h>
#include <cctype>
#include <cmath>

namespace UI {

namespace {
constexpr int ERROR_ACCOUNT_SCHEDULED_FOR_DELETION = 20011;
constexpr int ERROR_ACCOUNT_DISABLED = 20013;
constexpr int ERROR_PHONE_LOGIN_UNVERIFIED = 70007;

constexpr float FIELD_W = 280.0f;
constexpr float FIELD_X = (BOTTOM_SCREEN_WIDTH - FIELD_W) / 2.0f;
constexpr float FIELD_H = 28.0f;
constexpr float LABEL_Y = 28.0f;
constexpr float COUNTRY_Y = 46.0f;
constexpr float COUNTRY_H = 24.0f;
constexpr float LOGIN_BUTTON_H = 34.0f;

constexpr float PROMPT_W = 260.0f;
constexpr float PROMPT_H = 170.0f;
constexpr float PROMPT_X = (BOTTOM_SCREEN_WIDTH - PROMPT_W) / 2.0f;
constexpr float PROMPT_Y = (BOTTOM_SCREEN_HEIGHT - PROMPT_H) / 2.0f;
constexpr float PROMPT_FIELD_X = PROMPT_X + 20.0f;
constexpr float PROMPT_FIELD_W = PROMPT_W - 40.0f;
constexpr float PROMPT_INPUT_Y = PROMPT_Y + 52.0f;
constexpr float PROMPT_BUTTON_Y = PROMPT_Y + 92.0f;
constexpr float PROMPT_LINK_Y = PROMPT_Y + 130.0f;

bool touched(const touchPosition &touch, float x, float y, float w, float h) {
	return touch.px >= x && touch.px <= x + w && touch.py >= y && touch.py <= y + h;
}

struct FormLayout {
	float identifierY;
	float passwordY;
	float loginButtonY;
};

FormLayout formLayout(bool phoneLogin) {
	const float identifierY = phoneLogin ? COUNTRY_Y + COUNTRY_H + 4.0f : COUNTRY_Y;
	const float passwordY = identifierY + FIELD_H + 22.0f;
	return {identifierY, passwordY, passwordY + FIELD_H + 12.0f};
}

std::string loginModeSwitchText(bool phoneLogin) { return TR(phoneLogin ? "login.use_email" : "login.use_phone"); }
} // namespace

LoginScreen::LoginScreen() : qrCodeSize(0), qrCodeGenerated(false), loadingAngle(0.0f), animTimer(0.0f) {
	statusMessage = Core::I18n::getInstance().get("login.status.initializing");
}

LoginScreen::~LoginScreen() {}

void LoginScreen::onEnter() {
	Logger::log("[LoginScreen] Entered");

	checkTokenFile();

	Discord::RemoteAuth::getInstance().setOnStateChange(
	    [this](Discord::RemoteAuthState state, const std::string &info) { onStateChange(state, info); });

	Discord::RemoteAuth::getInstance().setOnUserScanned(
	    [this](const Discord::RemoteAuthUser &user) { onUserScanned(user); });

	Discord::RemoteAuth::getInstance().setOnTokenReceived([this](const std::string &token) { onTokenReceived(token); });

	bool hasToken = !Config::getInstance().getToken().empty();
	auto type = ScreenManager::getInstance().getCurrentType();

	bool isAlreadyConnected = Discord::DiscordClient::getInstance().getState() == Discord::ConnectionState::READY;

	if (type == ScreenType::ADD_ACCOUNT) {
		startQRLogin();
	} else if (!hasToken && !isAlreadyConnected) {
		startQRLogin();
	}

	if (type == ScreenType::ADD_ACCOUNT &&
	    Discord::DiscordClient::getInstance().getState() == Discord::ConnectionState::READY) {
		ignoreInitialConnection = true;
	} else {
		ignoreInitialConnection = false;
	}
}

void LoginScreen::onExit() {
	Logger::log("[LoginScreen] Exited");
	Discord::RemoteAuth::getInstance().setOnStateChange(nullptr);
	Discord::RemoteAuth::getInstance().setOnUserScanned(nullptr);
	Discord::RemoteAuth::getInstance().setOnTokenReceived(nullptr);
	Discord::RemoteAuth::getInstance().cancel();
}

void LoginScreen::update() {
	u32 kDown = hidKeysDown();

	if ((kDown & KEY_TOUCH) && !countryPicker.isOpen()) {
		touchPosition touch;
		hidTouchRead(&touch);
		if (touch.px < 35 && touch.py < 35) {
			ScreenManager::getInstance().pushScreen(ScreenType::SETTINGS);
			return;
		}
	}

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
	bool isLoading =
	    (state == Discord::ConnectionState::CONNECTING || state == Discord::ConnectionState::AUTHENTICATING);

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

	if (client.wasAuthFailed()) {
		client.clearAuthFailed();
		auto &cfg = Config::getInstance();
		cfg.removeAccount(cfg.getCurrentAccountIndex());
		if (cfg.getToken().empty()) {
			startQRLogin();
		}
		return;
	}

	bool shouldAutoConnect =
	    !Config::getInstance().getToken().empty() &&
	    (Discord::RemoteAuth::getInstance().getState() != Discord::RemoteAuthState::CONNECTING &&
	     Discord::RemoteAuth::getInstance().getState() != Discord::RemoteAuthState::WAITING_FOR_SCAN &&
	     Discord::RemoteAuth::getInstance().getState() != Discord::RemoteAuthState::WAITING_FOR_CONFIRM);
	auto screenType = ScreenManager::getInstance().getCurrentType();

	if (screenType == ScreenType::ADD_ACCOUNT) {
		shouldAutoConnect = false;
	}

	if ((screenType == ScreenType::LOGIN || screenType == ScreenType::ADD_ACCOUNT) && shouldAutoConnect) {
		if (client.getState() == Discord::ConnectionState::DISCONNECTED) {
			client.connect(Config::getInstance().getToken());
		}

		statusMessage = Discord::DiscordClient::getInstance().getStatusMessage();

		if (screenType == ScreenType::LOGIN) {
			return;
		}
	}

	Discord::RemoteAuth::getInstance().poll();

	if (prompt != Prompt::NONE) {
		if (kDown & KEY_B) {
			prompt = Prompt::NONE;
		} else if (kDown & KEY_TOUCH) {
			touchPosition touch;
			hidTouchRead(&touch);
			updatePrompt(touch);
		}
		return;
	}

	if (countryPicker.isOpen()) {
		countryPicker.update();
		return;
	}

	if ((kDown & KEY_B) && ScreenManager::getInstance().getCurrentType() == ScreenType::ADD_ACCOUNT) {
		if (Discord::DiscordClient::getInstance().getState() == Discord::ConnectionState::READY) {
			ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
			return;
		}
	}

	if (kDown & KEY_TOUCH) {
		touchPosition touch;
		hidTouchRead(&touch);

		const FormLayout layout = formLayout(phoneLogin);
		const float switchW = measureText(loginModeSwitchText(phoneLogin), 0.45f, 0.45f);

		if (touched(touch, FIELD_X + FIELD_W - switchW - 8.0f, LABEL_Y - 6.0f, switchW + 8.0f, 22.0f)) {
			phoneLogin = !phoneLogin;
		} else if (phoneLogin && touched(touch, FIELD_X, COUNTRY_Y, FIELD_W, COUNTRY_H)) {
			countryPicker.show();
		} else if (phoneLogin && touched(touch, FIELD_X, layout.identifierY, FIELD_W, FIELD_H)) {
			SwkbdState swkbd;
			swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 2, 16);
			swkbdSetNumpadKeys(&swkbd, '+', 0);
			swkbdSetHintText(&swkbd, TR("login.swkbd.phone").c_str());
			swkbdSetInitialText(&swkbd, phone.c_str());
			swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
			char buf[32];
			if (swkbdInputText(&swkbd, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
				phone = buf;
			}
		} else if (touched(touch, FIELD_X, layout.identifierY, FIELD_W, FIELD_H)) {
			SwkbdState swkbd;
			swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, -1);
			swkbdSetHintText(&swkbd, TR("login.swkbd.email").c_str());
			swkbdSetInitialText(&swkbd, email.c_str());
			swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN | SWKBD_DEFAULT_QWERTY);
			char buf[256];
			if (swkbdInputText(&swkbd, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
				email = buf;
			}
		} else if (touched(touch, FIELD_X, layout.passwordY, FIELD_W, FIELD_H)) {
			SwkbdState swkbd;
			swkbdInit(&swkbd, SWKBD_TYPE_WESTERN, 2, -1);
			swkbdSetHintText(&swkbd, TR("login.swkbd.password").c_str());
			swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN | SWKBD_DEFAULT_QWERTY);
			swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
			char buf[256];
			if (swkbdInputText(&swkbd, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
				password = buf;
			}
		} else if (touched(touch, FIELD_X, layout.loginButtonY, FIELD_W, LOGIN_BUTTON_H)) {
			if (!(phoneLogin ? phone : email).empty() && !password.empty()) {
				submitLogin(false);
			} else {
				statusMessage = TR(phoneLogin ? "login.error.empty_phone" : "login.error.empty");
			}
		}
	}
}

void LoginScreen::drawLoadingSpinner(float centerX, float centerY, float radius) {
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
	bool isLoading =
	    (state == Discord::ConnectionState::CONNECTING || state == Discord::ConnectionState::AUTHENTICATING);

	float centerX = TOP_SCREEN_WIDTH / 2.0f;
	float centerY = TOP_SCREEN_HEIGHT / 2.0f;

	if (isLoading) {
		C3D_Tex *discordTex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord.png", true);
		if (discordTex) {
			UI::ImageManager::ImageInfo info = UI::ImageManager::getInstance().getImageInfo("romfs:/discord.png");
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
			C2D_DrawImageAtRotated(img, centerX, centerY, 0.6f, rad, nullptr, scale, scale);
		} else {
			drawLoadingSpinner(centerX, centerY, 20.0f);
		}

		std::string status = Discord::DiscordClient::getInstance().getStatusMessage();
		if (status.empty()) {
			status = Core::I18n::getInstance().get("login.status.authenticating");
		}
		drawCenteredText(centerY + 60.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorText(), status, TOP_SCREEN_WIDTH);
		return;
	}

	float layoutY = 40.0f;

	float leftX = 30.0f;

	drawText(leftX, layoutY + 45.0f, 0.3f, 0.7f, 0.7f, ScreenManager::colorText(),
	         Core::I18n::getInstance().get("login.welcome"));

	drawRichText(leftX, layoutY + 75.0f, 0.3f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
	             Core::I18n::getInstance().get("login.excited"));

	drawText(leftX, layoutY + 125.0f, 0.3f, 0.38f, 0.38f, ScreenManager::colorTextMuted(), TR("login.token_hint"));

	float qrSize = 110.0f;
	float qrX = 285.0f;
	float qrY = 45.0f;

	float qrDrawX = qrX - (qrSize / 2.0f);

	if (qrCodeGenerated) {
		drawRoundedRect(qrDrawX - 8, qrY - 8, 0.25f, qrSize + 16, qrSize + 16, 8.0f, ScreenManager::colorWhite());
		drawQRCode(qrDrawX, qrY, qrSize);
	} else {
		drawRoundedRect(qrDrawX - 8, qrY - 8, 0.25f, qrSize + 16, qrSize + 16, 8.0f, ScreenManager::colorInput());

		std::string s = TR("login.generating");
		drawText(qrX - (measureText(s, 0.45f, 0.45f) / 2.0f), qrY + (qrSize / 2.0f) - 6.0f, 0.3f, 0.45f, 0.45f,
		         ScreenManager::colorTextMuted(), s);
	}

	float textY = qrY + qrSize + 18.0f;

	std::string t1 = Core::I18n::getInstance().get("login.qr_title");
	drawText(qrX - (measureText(t1, 0.5f, 0.5f) / 2.0f), textY, 0.3f, 0.5f, 0.5f, ScreenManager::colorText(), t1);

	std::string t2 = Core::I18n::getInstance().get("login.qr_subtitle");
	drawText(qrX - (measureText(t2, 0.38f, 0.38f) / 2.0f), textY + 18.0f, 0.3f, 0.38f, 0.38f,
	         ScreenManager::colorTextMuted(), t2);
}

void LoginScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_TargetClear(target, ScreenManager::colorBackgroundDark());
	C2D_SceneBegin(target);

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	auto state = client.getState();
	bool isLoading =
	    (state == Discord::ConnectionState::CONNECTING || state == Discord::ConnectionState::AUTHENTICATING);

	if (isLoading) {
		std::string warnText = TR("login.loading.many_servers_warning");
		std::vector<std::string> warnLines;
		std::string::size_type pos = 0, found;
		while ((found = warnText.find('\n', pos)) != std::string::npos) {
			warnLines.push_back(warnText.substr(pos, found - pos));
			pos = found + 1;
		}
		warnLines.push_back(warnText.substr(pos));
		float warnY = BOTTOM_SCREEN_HEIGHT - 10.0f - (float)warnLines.size() * 14.0f;
		for (const auto &line : warnLines) {
			drawCenteredText(warnY, 0.5f, 0.40f, 0.40f, ScreenManager::colorTextMuted(), line, BOTTOM_SCREEN_WIDTH);
			warnY += 14.0f;
		}
	}

	if (!isLoading) {

		if (prompt != Prompt::NONE) {
			renderPrompt();
		} else if (countryPicker.isOpen()) {
			countryPicker.render();
		} else {

			const FormLayout layout = formLayout(phoneLogin);

			drawText(FIELD_X, LABEL_Y, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
			         TR(phoneLogin ? "login.field.phone" : "login.field.email"));
			const std::string switchText = loginModeSwitchText(phoneLogin);
			drawText(FIELD_X + FIELD_W - measureText(switchText, 0.45f, 0.45f), LABEL_Y, 0.5f, 0.45f, 0.45f,
			         ScreenManager::colorSelection(), switchText);

			if (phoneLogin) {
				const Country &country = countryPicker.selected();
				const std::string dialCode = std::string("+") + country.dialCode;
				const float dialCodeW = measureText(dialCode, 0.45f, 0.45f);
				const float arrowX = FIELD_X + FIELD_W - 18.0f;
				drawRoundedRect(FIELD_X, COUNTRY_Y, 0.5f, FIELD_W, COUNTRY_H, 6.0f, ScreenManager::colorInput());
				drawText(FIELD_X + 8.0f, COUNTRY_Y + 5.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorText(), dialCode);
				drawText(FIELD_X + 16.0f + dialCodeW, COUNTRY_Y + 5.0f, 0.5f, 0.45f, 0.45f,
				         ScreenManager::colorTextMuted(),
				         getTruncatedText(country.name, arrowX - FIELD_X - 24.0f - dialCodeW, 0.45f, 0.45f));
				drawText(arrowX, COUNTRY_Y + 6.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), "\u25BC");
			}

			const std::string &identifier = phoneLogin ? phone : email;
			drawRoundedRect(FIELD_X, layout.identifierY, 0.5f, FIELD_W, FIELD_H, 6.0f, ScreenManager::colorInput());
			std::string identifierDisplay =
			    identifier.empty() ? TR(phoneLogin ? "login.field.phone_hint" : "login.field.email_hint") : identifier;
			u32 identifierColor = identifier.empty() ? ScreenManager::colorTextMuted() : ScreenManager::colorText();
			drawText(FIELD_X + 8.0f, layout.identifierY + 6.5f, 0.5f, 0.45f, 0.45f, identifierColor, identifierDisplay);

			drawRoundedRect(FIELD_X, layout.passwordY, 0.5f, FIELD_W, FIELD_H, 6.0f, ScreenManager::colorInput());
			drawText(FIELD_X, layout.passwordY - 18.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorTextMuted(),
			         TR("login.field.password"));
			std::string passDisplay =
			    password.empty() ? TR("login.field.password_hint") : std::string(password.length(), '*');
			u32 passColor = password.empty() ? ScreenManager::colorTextMuted() : ScreenManager::colorText();
			drawText(FIELD_X + 8.0f, layout.passwordY + 6.5f, 0.5f, 0.45f, 0.45f, passColor, passDisplay);

			drawRoundedRect(FIELD_X, layout.loginButtonY, 0.5f, FIELD_W, LOGIN_BUTTON_H, 8.0f,
			                ScreenManager::colorSelection());
			drawCenteredText(layout.loginButtonY + 9.0f, 0.5f, 0.55f, 0.55f, ScreenManager::colorWhite(),
			                 TR("login.button.login"), BOTTOM_SCREEN_WIDTH);

			if (ScreenManager::getInstance().getCurrentType() == ScreenType::LOGIN) {
				C3D_Tex *settingTex =
				    ImageManager::getInstance().getLocalImage("romfs:/discord-icons/setting.png", true);
				if (settingTex) {
					ImageManager::ImageInfo info =
					    ImageManager::getInstance().getImageInfo("romfs:/discord-icons/setting.png");
					Tex3DS_SubTexture sub;
					sub.width = (u16)info.originalW;
					sub.height = (u16)info.originalH;
					sub.left = 0.0f;
					sub.top = 0.0f;
					sub.right = (float)info.originalW / settingTex->width;
					sub.bottom = (float)info.originalH / settingTex->height;
					C2D_Image img = {settingTex, &sub};
					float iconSize = 20.0f;
					float scale = iconSize / info.originalW;
					C2D_DrawImageAt(img, 8, 8, 0.5f, nullptr, scale, scale);
				}
			}

			drawCenteredText(BOTTOM_SCREEN_HEIGHT - 35.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorError(), statusMessage,
			                 BOTTOM_SCREEN_WIDTH);
		}
	}
}

void LoginScreen::checkTokenFile() {
	std::string tokenPath = std::string(CONFIG_DIR_PATH) + "/token.txt";
	auto buffer = Utils::File::readFile(tokenPath);
	if (buffer.empty()) {
		return;
	}

	std::string fileToken = Utils::String::trim(std::string(buffer.data()));

	if (!fileToken.empty()) {
		Logger::log("[LoginScreen] Found token.txt, using manual token");
		Config::getInstance().addAccount(Core::I18n::getInstance().get("login.account_name"), fileToken);
		Discord::DiscordClient::getInstance().logout();
		ScreenManager::getInstance().clearCaches();
		ScreenManager::getInstance().resetSelection();
	}

	remove(tokenPath.c_str());
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

bool LoginScreen::isAnimating() const {
	auto state = Discord::DiscordClient::getInstance().getState();
	return state == Discord::ConnectionState::CONNECTING || state == Discord::ConnectionState::AUTHENTICATING;
}

void LoginScreen::onStateChange(Discord::RemoteAuthState state, const std::string &info) {
	Utils::requestRedraw();
	statusMessage = info;

	if (state == Discord::RemoteAuthState::FAILED) {
		statusMessage = Core::I18n::getInstance().get("login.status.failed") + ": " + info;
	} else if (state == Discord::RemoteAuthState::CANCELLED) {
		statusMessage = Core::I18n::getInstance().get("common.cancel");
	}

	std::string url = Discord::RemoteAuth::getInstance().getQRCodeUrl();
	if (!url.empty() && url != qrCodeUrl) {
		generateQRCode(url);
	}
}

void LoginScreen::onUserScanned(const Discord::RemoteAuthUser &user) {
	Utils::requestRedraw();
	statusMessage = user.username.empty() ? Core::I18n::getInstance().get("login.status.scan_complete")
	                                      : Core::I18n::format(TR("login.status.scanned_by"), user.username);
}

bool LoginScreen::offersSms() const { return prompt == Prompt::MFA && mfaMethod != "sms" && mfaSmsAvailable; }

// QR login keeps writing statusMessage underneath an open prompt.
void LoginScreen::setLoginStatus(const std::string &message) {
	if (prompt != Prompt::NONE) {
		promptStatus = message;
	} else {
		statusMessage = message;
	}
}

// Discord only takes phone numbers in E.164.
std::string LoginScreen::loginIdentifier() const {
	if (!phoneLogin) {
		return email;
	}
	std::string digits;
	for (char c : phone) {
		if (isdigit((unsigned char)c)) {
			digits += c;
		}
	}
	if (!phone.empty() && phone[0] == '+') {
		return "+" + digits;
	}
	// The trunk prefix dialled before national numbers, as in 090 for +81 90.
	if (!digits.empty() && digits[0] == '0') {
		digits.erase(0, 1);
	}
	return std::string("+") + countryPicker.selected().dialCode + digits;
}

void LoginScreen::submitLogin(bool undelete) {
	setLoginStatus(TR("login.status.logging_in"));
	Discord::DiscordClient::getInstance().performLogin(
	    loginIdentifier(), password, undelete,
	    [this](const Discord::LoginResult &result) { handleLoginResult(result); });
}

void LoginScreen::handleLoginResult(const Discord::LoginResult &result) {
	if (result.success) {
		prompt = Prompt::NONE;
		onLoginSuccess(result.token);
		return;
	}

	promptCode.clear();
	if (result.mfaRequired) {
		prompt = Prompt::MFA;
		mfaTicket = result.ticket;
		mfaLoginInstanceId = result.loginInstanceId;
		mfaSmsAvailable = result.sms;
		mfaPhone.clear();
		mfaMethod = "totp";
		setLoginStatus(TR("login.mfa.required"));
		if (result.sms && !result.totp) {
			requestMfaSms();
		}
	} else if (result.errorCode == ERROR_ACCOUNT_SCHEDULED_FOR_DELETION || result.errorCode == ERROR_ACCOUNT_DISABLED) {
		prompt = Prompt::UNDELETE;
		setLoginStatus(result.error);
	} else if (result.errorCode == ERROR_PHONE_LOGIN_UNVERIFIED) {
		prompt = Prompt::PHONE_CODE;
		setLoginStatus(result.error);
	} else {
		prompt = Prompt::NONE;
		setLoginStatus(TR("login.status.failed") + ": " + result.error);
	}
}

void LoginScreen::requestMfaSms() {
	setLoginStatus(TR("login.mfa.sending_sms"));
	Discord::DiscordClient::getInstance().sendMfaSms(mfaTicket, [this](bool success, const std::string &phoneOrError) {
		if (success) {
			mfaMethod = "sms";
			mfaPhone = phoneOrError;
			promptCode.clear();
			setLoginStatus("");
		} else {
			setLoginStatus(TR("login.status.failed") + ": " + phoneOrError);
		}
	});
}

void LoginScreen::submitPrompt() {
	auto &client = Discord::DiscordClient::getInstance();
	if (prompt == Prompt::UNDELETE) {
		submitLogin(true);
		return;
	}
	if (promptCode.empty()) {
		return;
	}

	setLoginStatus(TR(prompt == Prompt::MFA ? "login.mfa.verifying" : "login.phone.verifying"));
	if (prompt == Prompt::MFA) {
		client.submitMFA(mfaMethod, mfaTicket, mfaLoginInstanceId, promptCode,
		                 [this](const Discord::LoginResult &result) {
			                 if (result.success) {
				                 prompt = Prompt::NONE;
				                 onLoginSuccess(result.token);
			                 } else {
				                 setLoginStatus(TR("login.status.failed") + ": " + result.error);
			                 }
		                 });
	} else {
		client.authorizeLoginByPhone(loginIdentifier(), promptCode, [this](const std::string &error) {
			if (error.empty()) {
				submitLogin(false);
			} else {
				setLoginStatus(TR("login.status.failed") + ": " + error);
			}
		});
	}
}

void LoginScreen::updatePrompt(const touchPosition &touch) {
	if (prompt != Prompt::UNDELETE && touched(touch, PROMPT_FIELD_X, PROMPT_INPUT_Y, PROMPT_FIELD_W, 30.0f)) {
		SwkbdState swkbd;
		swkbdInit(&swkbd, SWKBD_TYPE_NUMPAD, 1, 8);
		swkbdSetHintText(&swkbd, TR(prompt == Prompt::MFA ? "login.swkbd.mfa" : "login.swkbd.code").c_str());
		char buf[16];
		if (swkbdInputText(&swkbd, buf, sizeof(buf)) == SWKBD_BUTTON_CONFIRM) {
			promptCode = buf;
		}
	} else if (touched(touch, PROMPT_FIELD_X, PROMPT_BUTTON_Y, PROMPT_FIELD_W, 32.0f)) {
		submitPrompt();
	} else if (offersSms() && touched(touch, PROMPT_FIELD_X, PROMPT_LINK_Y - 4.0f, PROMPT_FIELD_W, 20.0f)) {
		requestMfaSms();
	}
}

void LoginScreen::renderPrompt() {
	std::string title;
	std::string desc;
	std::string button = TR("login.mfa.verify");
	if (prompt == Prompt::MFA) {
		title = TR("login.mfa.title");
		desc = mfaMethod == "sms" ? Core::I18n::format(TR("login.mfa.desc_sms"), mfaPhone) : TR("login.mfa.desc");
	} else if (prompt == Prompt::PHONE_CODE) {
		title = TR("login.phone.title");
		desc = TR("login.phone.desc");
	} else {
		title = TR("login.undelete.title");
		desc = TR("login.undelete.desc");
		button = TR("login.undelete.button");
	}

	drawOverlay(0.4f);
	drawPopupBackground(PROMPT_X, PROMPT_Y, PROMPT_W, PROMPT_H, 0.5f);

	drawCenteredText(PROMPT_Y + 12.0f, 0.6f, 0.55f, 0.55f, ScreenManager::colorText(), title, BOTTOM_SCREEN_WIDTH);
	drawCenteredText(PROMPT_Y + 32.0f, 0.6f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), desc, BOTTOM_SCREEN_WIDTH);

	if (prompt != Prompt::UNDELETE) {
		drawRoundedRect(PROMPT_FIELD_X, PROMPT_INPUT_Y, 0.6f, PROMPT_FIELD_W, 30.0f, 6.0f,
		                ScreenManager::colorBackgroundLight());
		std::string codeDisplay = promptCode.empty() ? TR("login.mfa.hint") : promptCode;
		drawText(PROMPT_FIELD_X + 5.0f, PROMPT_INPUT_Y + 7.5f, 0.7f, 0.45f, 0.45f, ScreenManager::colorText(),
		         codeDisplay);
	}

	drawRoundedRect(PROMPT_FIELD_X, PROMPT_BUTTON_Y, 0.6f, PROMPT_FIELD_W, 32.0f, 6.0f,
	                ScreenManager::colorSelection());
	drawCenteredText(PROMPT_BUTTON_Y + 8.5f, 0.7f, 0.5f, 0.5f, ScreenManager::colorWhite(), button,
	                 BOTTOM_SCREEN_WIDTH);

	if (offersSms()) {
		drawCenteredText(PROMPT_LINK_Y, 0.7f, 0.4f, 0.4f, ScreenManager::colorSelection(), TR("login.mfa.use_sms"),
		                 BOTTOM_SCREEN_WIDTH);
	}

	drawCenteredText(PROMPT_Y + 150.0f, 0.7f, 0.4f, 0.4f, ScreenManager::colorError(),
	                 getTruncatedText(promptStatus, PROMPT_W - 20.0f, 0.4f, 0.4f), BOTTOM_SCREEN_WIDTH);

	drawCenteredText(PROMPT_Y + PROMPT_H + 10.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
	                 TR("login.mfa.cancel"), BOTTOM_SCREEN_WIDTH);
}

void LoginScreen::onTokenReceived(const std::string &ticket) {
	Utils::requestRedraw();
	Logger::log("[LoginScreen] Ticket received (len %zu)", ticket.size());
	statusMessage = Core::I18n::getInstance().get("login.status.exchanging");

	Discord::DiscordClient::getInstance().exchangeTicketForToken(ticket, [this](const std::string &encryptedToken) {
		if (encryptedToken.empty()) {
			statusMessage = Core::I18n::getInstance().get("login.status.failed_exchange");
			qrCodeGenerated = false;
			return;
		}

		Logger::log("[LoginScreen] Encrypted token received (len %zu)", encryptedToken.size());

		std::string token = Discord::RemoteAuth::getInstance().decryptToken(encryptedToken);
		if (token.empty()) {
			statusMessage = Core::I18n::getInstance().get("login.status.failed_decrypt");
			qrCodeGenerated = false;
			return;
		}

		Logger::log("[LoginScreen] Token decrypted (len %zu)", token.size());

		onLoginSuccess(token);
	});
}

void LoginScreen::onLoginSuccess(const std::string &token) {
	Config::getInstance().addAccount(Core::I18n::getInstance().get("login.account_name"), token);

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

	bool ok = qrcodegen_encodeText(data.c_str(), tempBuffer, qrcode, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
	                               qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);

	if (!ok) {
		Logger::log("[LoginScreen] Failed to generate QR code");
		qrCodeGenerated = false;
		return;
	}

	qrCodeSize = qrcodegen_getSize(qrcode);
	Logger::log("[LoginScreen] QR code generated: %d x %d", qrCodeSize, qrCodeSize);

	qrCodeData.resize(qrCodeSize * qrCodeSize);
	for (int y = 0; y < qrCodeSize; y++) {
		for (int x = 0; x < qrCodeSize; x++) {
			qrCodeData[y * qrCodeSize + x] = qrcodegen_getModule(qrcode, x, y) ? 1 : 0;
		}
	}

	qrCodeGenerated = true;
	qrCodeUrl = data;
}

void LoginScreen::drawQRCode(float x, float y, float size) {
	if (!qrCodeGenerated || qrCodeSize == 0) {
		return;
	}

	float moduleSize = size / qrCodeSize;

	for (int row = 0; row < qrCodeSize; row++) {
		for (int col = 0; col < qrCodeSize; col++) {
			if (qrCodeData[row * qrCodeSize + col]) {

				C2D_DrawRectSolid(x + col * moduleSize, y + row * moduleSize, 0.3f, moduleSize, moduleSize,
				                  C2D_Color32(0, 0, 0, 255));
			}
		}
	}
}

} // namespace UI
