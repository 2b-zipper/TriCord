#ifndef CONFIG_H
#define CONFIG_H

#define DISCORD_GATEWAY_URL "wss://gateway.discord.gg/?v=10&encoding=json"
#define DISCORD_REMOTE_AUTH_URL "wss://remote-auth-gateway.discord.gg/?v=2"
#define DISCORD_QR_BASE_URL "https://discord.com/ra/"

#define APP_NAME "TriCord"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define APP_VERSION                                                            \
  TOSTRING(APP_VERSION_MAJOR)                                                  \
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
  u32 background;
  u32 backgroundDark;
  u32 backgroundLight;
  u32 primary;
  u32 text;
  u32 textMuted;
  u32 success;
  u32 error;
  u32 embed;
  u32 embedMedia;
  u32 reaction;
  u32 reactionMe;
  std::string name;
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
  void setTimezoneOffset(int offset) { timezoneOffset = offset; }

  std::string getLanguage() const { return language; }
  void setLanguage(const std::string &lang) {
    language = lang;
    Core::I18n::getInstance().loadLanguage(lang);
    saveSettings();
  }

  const Theme &getTheme() const;
  void setCustomTheme(const Theme &theme) {
    customTheme = theme;
    saveTheme();
  }
  int getThemeType() const { return themeType; }
  void setThemeType(int type) { themeType = type; }
  bool isTypingIndicatorEnabled() const { return typingIndicatorEnabled; }
  void setTypingIndicatorEnabled(bool enabled) {
    typingIndicatorEnabled = enabled;
  }
  bool isFileLoggingEnabled() const { return fileLoggingEnabled; }
  void setFileLoggingEnabled(bool enabled);

  void loadTheme();
  void saveTheme();

private:
  Config();
  std::vector<Account> accounts;
  int currentAccountIndex;
  int timezoneOffset;
  std::string language;
  int themeType;
  bool typingIndicatorEnabled;
  bool fileLoggingEnabled;
  Theme customTheme;

  mutable std::recursive_mutex mutex;

  static Theme getDarkPreset();
  static Theme getLightPreset();
};

#endif // CONFIG_H
