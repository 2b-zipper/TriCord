#ifndef CONFIG_H
#define CONFIG_H

#define DISCORD_GATEWAY_URL "wss://gateway.discord.gg/?v=10&encoding=json&compress=zlib-stream"
#define DISCORD_REMOTE_AUTH_URL "wss://remote-auth-gateway.discord.gg/?v=2"
#define DISCORD_QR_BASE_URL "https://discord.com/ra/"

#define APP_NAME "TriCord"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define APP_VERSION                                                                                                    \
	TOSTRING(APP_VERSION_MAJOR)                                                                                        \
	"." TOSTRING(APP_VERSION_MINOR) "." TOSTRING(APP_VERSION_MICRO)
#define APP_USER_AGENT "TriCord/" APP_VERSION " (Nintendo 3DS)"

#define HTTP_TIMEOUT_SECONDS 30

#define TOP_SCREEN_WIDTH 400
#define TOP_SCREEN_HEIGHT 240
#define BOTTOM_SCREEN_WIDTH 320
#define BOTTOM_SCREEN_HEIGHT 240

#define CONFIG_DIR_PATH "sdmc:/3ds/TriCord"

#include "core/i18n.h"
#include <3ds.h>
#include <mutex>
#include <string>
#include <vector>

struct Theme {
	u32 bg;
	u32 bg_dark;
	u32 bg_light;
	u32 accent;
	u32 selection;
	u32 separator;
	u32 header_border;
	u32 overlay;
	u32 pure_white;

	u32 text;
	u32 text_muted;
	u32 link;

	u32 embed_bg;
	u32 embed_media_bg;
	u32 reaction_bg;
	u32 reaction_me_bg;
	u32 input_bg;

	u32 success;
	u32 error;

	std::string name;
	std::string author;
	std::string description;
};

class Config {
  public:
	struct Account {
		std::string name;
		std::string token;
	};

	static Config &getInstance() {
		static Config instance;
		return instance;
	}

	void load();
	void save();

	void loadSettings();
	void saveSettings();

	std::string getToken() const;
	void setToken(const std::string &newToken);

	const std::vector<Account> &getAccounts() const { return accounts; }
	void addAccount(const std::string &name, const std::string &token);
	void removeAccount(int index);
	void selectAccount(int index);
	int getCurrentAccountIndex() const { return currentAccountIndex; }
	void updateCurrentAccountName(const std::string &name);

	bool hasToken() const { return !accounts.empty(); }

	int getTimezoneOffset() const { return timezoneOffset; }
	void setTimezoneOffset(int offset);

	std::string getLanguage() const { return language; }
	void setLanguage(const std::string &lang) {
		language = lang;
		Core::I18n::getInstance().loadLanguage(lang);
		saveSettings();
	}

	const Theme &getTheme() const;
	void setCustomTheme(const Theme &theme) { customTheme = theme; }

	bool isCustomThemeEnabled() const { return customThemeEnabled; }
	void setCustomThemeEnabled(bool enabled);

	std::string getSelectedThemeName() const { return selectedThemeName; }
	void setSelectedThemeName(const std::string &name);

	int getThemeType() const { return themeType; }
	void setThemeType(int type);
	bool isTypingIndicatorEnabled() const { return typingIndicatorEnabled; }
	void setTypingIndicatorEnabled(bool enabled);
	bool isFileLoggingEnabled() const { return fileLoggingEnabled; }
	void setFileLoggingEnabled(bool enabled);

	bool isShowAvatarsEnabled() const { return showAvatars; }
	void setShowAvatarsEnabled(bool enabled);
	bool isShowServerIconsEnabled() const { return showServerIcons; }
	void setShowServerIconsEnabled(bool enabled);
	bool isShowHiddenChannelsEnabled() const { return showHiddenChannels; }
	void setShowHiddenChannelsEnabled(bool enabled);

	bool isDisclaimerAccepted() const { return disclaimerAccepted; }
	void setDisclaimerAccepted(bool accepted);

	bool isSslVerificationDisabled() const { return sslVerificationDisabled; }
	void setSslVerificationDisabled(bool disabled);

	void loadTheme();

	std::vector<std::string> getAvailableThemes();
	bool loadThemeFromFile(const std::string &name);
	void deleteTheme(const std::string &name);

  private:
	Config();
	std::vector<Account> accounts;
	int currentAccountIndex;
	int timezoneOffset;
	std::string language;
	int themeType;
	bool typingIndicatorEnabled;
	bool fileLoggingEnabled;
	bool disclaimerAccepted;
	bool sslVerificationDisabled;
	bool showAvatars;
	bool showServerIcons;
	bool showHiddenChannels;

	bool customThemeEnabled;
	std::string selectedThemeName;
	Theme customTheme;

	mutable std::recursive_mutex mutex;

	static Theme getDarkPreset();
	static Theme getLightPreset();
};

#endif // CONFIG_H
