#include "config.h"

#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "log.h"
#include "utils/color_utils.h"
#include "utils/file_utils.h"

Config::Config()
    : currentAccountIndex(-1), timezoneOffset(0), language("en_US"), themeType(0), typingIndicatorEnabled(true),
      fileLoggingEnabled(false), disclaimerAccepted(false), sslVerificationDisabled(false), showAvatars(true),
      showServerIcons(true), customThemeEnabled(false), selectedThemeName("") {
	customTheme = getDarkPreset();
	customTheme.name = "Custom Theme";
}

Theme Config::getDarkPreset() {
	Theme t;
	t.name = "Dark Mode";
	t.author = "TriCord Team";
	t.bg = 0xFF383331;
	t.bg_dark = 0xFF312D2B;
	t.bg_light = 0xFF494240;
	t.accent = 0xFFF26558;
	t.selection = 0xFFF26558;
	t.separator = 0xFFA49B94;
	t.header_border = 0x1EFFFFFF;
	t.overlay = 0x96000000;
	t.pure_white = 0xFFFFFFFF;

	t.text = 0xFFFFFFFF;
	t.text_muted = 0xFFA49B94;
	t.link = 0xFFFEBA49;

	t.embed_bg = 0xFF312D2B;
	t.embed_media_bg = 0xFF383331;
	t.reaction_bg = 0xFF494240;
	t.reaction_me_bg = 0xFF8B6447;
	t.input_bg = 0xFF252220;

	t.success = 0xFF6DB143;
	t.error = 0xFF4D47F0;
	return t;
}

Theme Config::getLightPreset() {
	Theme t;
	t.name = "Light Mode";
	t.author = "TriCord Team";
	t.bg = 0xFFFFFFFF;
	t.bg_dark = 0xFFF5F3F2;
	t.bg_light = 0xFFE5E2E0;
	t.accent = 0xFFF26558;
	t.selection = 0xFFF26558;
	t.separator = 0xFF58504E;
	t.header_border = 0x1EFFFFFF;
	t.overlay = 0x96000000;
	t.pure_white = 0xFFFFFFFF;

	t.text = 0xFF070606;
	t.text_muted = 0xFF58504E;
	t.link = 0xFFFEBA49;

	t.embed_bg = 0xFFFBFBFB;
	t.embed_media_bg = 0xFFF5F3F2;
	t.reaction_bg = 0xFFE0E2E5;
	t.reaction_me_bg = 0xFFFAEAED;
	t.input_bg = 0xFFE5E2E0;

	t.success = 0xFF6DB143;
	t.error = 0xFF4D47F0;
	return t;
}

const Theme &Config::getTheme() const {
	if (customThemeEnabled) {
		return customTheme;
	}
	if (themeType == 0) {
		static Theme dark = getDarkPreset();
		return dark;
	} else if (themeType == 1) {
		static Theme light = getLightPreset();
		return light;
	}
	return customTheme;
}
static void encrypt_decrypt_data(std::vector<u8> &data) {
	size_t original_size = data.size();
	size_t padded_size = (original_size + 15) & ~15;
	data.resize(padded_size, 0);

	u8 iv[16];
	memset(iv, 0, sizeof(iv));

	PS_EncryptDecryptAes(padded_size, data.data(), data.data(), PS_ALGORITHM_CTR_ENC, PS_KEYSLOT_0D, iv);
}

void Config::load() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	accounts.clear();
	struct stat st = {0};
	if (stat(CONFIG_DIR_PATH, &st) == -1) {
		mkdir(CONFIG_DIR_PATH, 0700);
	}

	std::string themesPath = std::string(CONFIG_DIR_PATH) + "/themes";
	if (stat(themesPath.c_str(), &st) == -1) {
		mkdir(themesPath.c_str(), 0700);
	}

	std::string accountsPath = std::string(CONFIG_DIR_PATH) + "/accounts";
	std::vector<unsigned char> buffer = Utils::File::readFileBinary(accountsPath);
	if (!buffer.empty()) {
		encrypt_decrypt_data(buffer);
		buffer.push_back('\0');

		rapidjson::Document doc;
		doc.Parse((char *)buffer.data());

		if (!doc.HasParseError() && doc.IsObject()) {
			if (doc.HasMember("currentIndex") && doc["currentIndex"].IsInt()) {
				currentAccountIndex = doc["currentIndex"].GetInt();
			}

			if (doc.HasMember("accounts") && doc["accounts"].IsArray()) {
				const auto &arr = doc["accounts"];
				for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
					const auto &obj = arr[i];
					if (obj.IsObject() && obj.HasMember("token") && obj["token"].IsString()) {
						Account acc;
						acc.token = obj["token"].GetString();
						if (obj.HasMember("name") && obj["name"].IsString()) {
							acc.name = obj["name"].GetString();
						} else {
							acc.name = "Account " + std::to_string(i + 1);
						}
						accounts.push_back(acc);
					}
				}
			}
		}
	}

	loadSettings();

	if (accounts.empty()) {
		currentAccountIndex = -1;
	} else if (currentAccountIndex < 0 || currentAccountIndex >= (int)accounts.size()) {
		currentAccountIndex = 0;
	}
}

void Config::save() {
	std::lock_guard<std::recursive_mutex> lock(mutex);
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);

	writer.StartObject();
	writer.Key("currentIndex");
	writer.Int(currentAccountIndex);

	writer.Key("accounts");
	writer.StartArray();
	for (const auto &acc : accounts) {
		writer.StartObject();
		writer.Key("name");
		writer.String(acc.name.c_str());
		writer.Key("token");
		writer.String(acc.token.c_str());
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();

	std::string jsonStr = s.GetString();
	std::vector<u8> data(jsonStr.begin(), jsonStr.end());
	encrypt_decrypt_data(data);

	std::string accountsPath = std::string(CONFIG_DIR_PATH) + "/accounts";
	Utils::File::writeFile(accountsPath, data);
}

void Config::loadSettings() {
	std::string settingsPath = std::string(CONFIG_DIR_PATH) + "/settings.json";
	std::vector<char> buffer = Utils::File::readFile(settingsPath);

	if (!buffer.empty()) {
		rapidjson::Document doc;
		doc.Parse(buffer.data());

		if (!doc.HasParseError() && doc.IsObject()) {
			if (doc.HasMember("timezone_offset") && doc["timezone_offset"].IsInt()) {
				timezoneOffset = doc["timezone_offset"].GetInt();
			}
			if (doc.HasMember("theme_type") && doc["theme_type"].IsInt()) {
				themeType = doc["theme_type"].GetInt();
			}
			if (doc.HasMember("language") && doc["language"].IsString()) {
				language = doc["language"].GetString();
			}
			if (doc.HasMember("typing_indicator") && doc["typing_indicator"].IsBool()) {
				typingIndicatorEnabled = doc["typing_indicator"].GetBool();
			}
			if (doc.HasMember("file_logging") && doc["file_logging"].IsBool()) {
				fileLoggingEnabled = doc["file_logging"].GetBool();
			}
			if (doc.HasMember("disclaimer_accepted") && doc["disclaimer_accepted"].IsBool()) {
				disclaimerAccepted = doc["disclaimer_accepted"].GetBool();
			}
			if (doc.HasMember("ssl_verification_disabled") && doc["ssl_verification_disabled"].IsBool()) {
				sslVerificationDisabled = doc["ssl_verification_disabled"].GetBool();
			}
			if (doc.HasMember("custom_theme_enabled") && doc["custom_theme_enabled"].IsBool()) {
				customThemeEnabled = doc["custom_theme_enabled"].GetBool();
			}
			if (doc.HasMember("selected_theme_name") && doc["selected_theme_name"].IsString()) {
				selectedThemeName = doc["selected_theme_name"].GetString();
			}
			if (doc.HasMember("show_avatars") && doc["show_avatars"].IsBool()) {
				showAvatars = doc["show_avatars"].GetBool();
			}
			if (doc.HasMember("show_server_icons") && doc["show_server_icons"].IsBool()) {
				showServerIcons = doc["show_server_icons"].GetBool();
			}
		} else {
			saveSettings();
		}
	} else {
		saveSettings();
	}

	if (!Core::I18n::getInstance().loadLanguage(language.empty() ? "en_US" : language)) {
		Core::I18n::getInstance().loadLanguage("en_US");
	}
	Logger::setFileLoggingEnabled(fileLoggingEnabled);
}

void Config::saveSettings() {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);

	writer.StartObject();
	writer.Key("timezone_offset");
	writer.Int(timezoneOffset);
	writer.Key("theme_type");
	writer.Int(themeType);
	writer.Key("language");
	writer.String(language.c_str());
	writer.Key("typing_indicator");
	writer.Bool(typingIndicatorEnabled);
	writer.Key("file_logging");
	writer.Bool(fileLoggingEnabled);
	writer.Key("disclaimer_accepted");
	writer.Bool(disclaimerAccepted);
	writer.Key("ssl_verification_disabled");
	writer.Bool(sslVerificationDisabled);
	writer.Key("custom_theme_enabled");
	writer.Bool(customThemeEnabled);
	writer.Key("selected_theme_name");
	writer.String(selectedThemeName.c_str());
	writer.Key("show_avatars");
	writer.Bool(showAvatars);
	writer.Key("show_server_icons");
	writer.Bool(showServerIcons);
	writer.EndObject();

	std::string settingsPath = std::string(CONFIG_DIR_PATH) + "/settings.json";
	Utils::File::writeFile(settingsPath, s.GetString());
}

void Config::setFileLoggingEnabled(bool enabled) {
	fileLoggingEnabled = enabled;
	Logger::setFileLoggingEnabled(enabled);
	saveSettings();
}

void Config::setDisclaimerAccepted(bool accepted) {
	disclaimerAccepted = accepted;
	saveSettings();
}

void Config::setSslVerificationDisabled(bool disabled) {
	sslVerificationDisabled = disabled;
	saveSettings();
}

void Config::setTimezoneOffset(int offset) {
	timezoneOffset = offset;
	saveSettings();
}

void Config::setThemeType(int type) {
	themeType = type;
	if (customThemeEnabled && !selectedThemeName.empty()) {
		loadThemeFromFile(selectedThemeName);
	}
	saveSettings();
}

void Config::setTypingIndicatorEnabled(bool enabled) {
	typingIndicatorEnabled = enabled;
	saveSettings();
}

void Config::setShowAvatarsEnabled(bool enabled) {
	showAvatars = enabled;
	saveSettings();
}

void Config::setShowServerIconsEnabled(bool enabled) {
	showServerIcons = enabled;
	saveSettings();
}

std::string Config::getToken() const {
	std::lock_guard<std::recursive_mutex> lock(const_cast<std::recursive_mutex &>(mutex));
	if (currentAccountIndex >= 0 && currentAccountIndex < (int)accounts.size()) {
		return accounts[currentAccountIndex].token;
	}
	return "";
}

void Config::setToken(const std::string &newToken) { addAccount("New Account", newToken); }

void Config::addAccount(const std::string &name, const std::string &token) {
	std::lock_guard<std::recursive_mutex> lock(mutex);

	for (size_t i = 0; i < accounts.size(); i++) {
		if (accounts[i].token == token) {
			currentAccountIndex = (int)i;
			save();
			return;
		}
	}

	Account acc;
	acc.name = name;
	acc.token = token;
	accounts.push_back(acc);
	currentAccountIndex = (int)accounts.size() - 1;
	save();
}

void Config::removeAccount(int index) {
	if (index >= 0 && index < (int)accounts.size()) {
		accounts.erase(accounts.begin() + index);
		if (accounts.empty()) {
			currentAccountIndex = -1;
		} else {
			if (currentAccountIndex == index) {
				if (currentAccountIndex >= (int)accounts.size()) {
					currentAccountIndex = (int)accounts.size() - 1;
				}
			} else if (currentAccountIndex > index) {
				currentAccountIndex--;
			}
		}
		save();
	}
}

void Config::selectAccount(int index) {
	if (index >= -1 && index < (int)accounts.size()) {
		currentAccountIndex = index;
		save();
	}
}

void Config::updateCurrentAccountName(const std::string &name) {
	if (currentAccountIndex >= 0 && currentAccountIndex < (int)accounts.size()) {
		accounts[currentAccountIndex].name = name;
		save();
	}
}

void Config::loadTheme() {
	customTheme = (themeType == 1) ? getLightPreset() : getDarkPreset();
	if (customThemeEnabled && !selectedThemeName.empty()) {
		loadThemeFromFile(selectedThemeName);
	}
}

void Config::setCustomThemeEnabled(bool enabled) {
	customThemeEnabled = enabled;
	saveSettings();
}

void Config::setSelectedThemeName(const std::string &name) {
	selectedThemeName = name;
	saveSettings();
}

std::vector<std::string> Config::getAvailableThemes() {
	std::vector<std::string> themes;
	std::string themesPath = std::string(CONFIG_DIR_PATH) + "/themes";
	struct dirent *ent;
	DIR *dir = opendir(themesPath.c_str());
	if (dir != NULL) {
		while ((ent = readdir(dir)) != NULL) {
			std::string name = ent->d_name;
			if (name.length() > 5 && name.substr(name.length() - 5) == ".json") {
				themes.push_back(name.substr(0, name.length() - 5));
			}
		}
		closedir(dir);
	}
	return themes;
}

bool Config::loadThemeFromFile(const std::string &name) {
	std::string path = std::string(CONFIG_DIR_PATH) + "/themes/" + name + ".json";
	std::vector<char> buffer = Utils::File::readFile(path);
	if (buffer.empty()) {
		return false;
	}

	rapidjson::Document doc;
	doc.Parse(buffer.data());
	if (doc.HasParseError() || !doc.IsObject()) {
		return false;
	}

	Theme newTheme = (themeType == 1) ? getLightPreset() : getDarkPreset();
	if (doc.HasMember("name") && doc["name"].IsString()) {
		newTheme.name = doc["name"].GetString();
	} else {
		newTheme.name = name;
	}

	if (doc.HasMember("author") && doc["author"].IsString()) {
		newTheme.author = doc["author"].GetString();
	} else {
		newTheme.author = "";
	}

	if (doc.HasMember("description") && doc["description"].IsString()) {
		newTheme.description = doc["description"].GetString();
	} else {
		newTheme.description = "";
	}

	auto loadCol = [&](const char *category, const char *key, u32 &target) {
		if (doc.HasMember("colors") && doc["colors"].IsObject()) {
			const auto &colors = doc["colors"];
			if (colors.HasMember(category) && colors[category].IsObject()) {
				const auto &cat = colors[category];
				if (cat.HasMember(key) && cat[key].IsString()) {
					target = Utils::Color::hexToColor(cat[key].GetString());
				}
			}
		}
	};

	loadCol("ui", "background", newTheme.bg);
	loadCol("ui", "background_dark", newTheme.bg_dark);
	loadCol("ui", "background_light", newTheme.bg_light);
	loadCol("ui", "accent", newTheme.accent);
	loadCol("ui", "selection", newTheme.selection);
	loadCol("ui", "separator", newTheme.separator);
	loadCol("ui", "header_border", newTheme.header_border);
	loadCol("ui", "overlay", newTheme.overlay);
	loadCol("ui", "pure_white", newTheme.pure_white);

	loadCol("text", "main", newTheme.text);
	loadCol("text", "muted", newTheme.text_muted);
	loadCol("text", "link", newTheme.link);

	loadCol("discord", "embed_bg", newTheme.embed_bg);
	loadCol("discord", "embed_media_bg", newTheme.embed_media_bg);
	loadCol("discord", "reaction_bg", newTheme.reaction_bg);
	loadCol("discord", "reaction_me_bg", newTheme.reaction_me_bg);
	loadCol("discord", "input_bg", newTheme.input_bg);

	loadCol("status", "success", newTheme.success);
	loadCol("status", "error", newTheme.error);

	customTheme = newTheme;
	selectedThemeName = name;
	saveSettings();
	return true;
}

void Config::deleteTheme(const std::string &name) {
	std::string path = std::string(CONFIG_DIR_PATH) + "/themes/" + name + ".json";
	remove(path.c_str());
	if (selectedThemeName == name) {
		selectedThemeName = "";
		customThemeEnabled = false;
		saveSettings();
	}
}
