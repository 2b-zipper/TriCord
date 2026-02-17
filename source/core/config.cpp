#include "config.h"

#include <3ds.h>
#include <citro2d.h>
#include <cstdio>
#include <cstring>
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
    : currentAccountIndex(-1), timezoneOffset(0), language("en"), themeType(0),
      typingIndicatorEnabled(true), fileLoggingEnabled(false) {
  customTheme = getDarkPreset();
  customTheme.name = "Custom Theme";
}

Theme Config::getDarkPreset() {
  Theme t;
  t.name = "Dark Mode";
  t.background = 0xFF383331;
  t.backgroundDark = 0xFF312D2B;
  t.backgroundLight = 0xFF494240;
  t.primary = 0xFFF26558;
  t.text = 0xFFFFFFFF;
  t.textMuted = 0xFFA49B94;
  t.success = 0xFF6DB143;
  t.error = 0xFF4D47F0;
  t.embed = 0xFF312D2B;
  t.embedMedia = 0xFF383331;
  t.reaction = 0xFF494240;
  t.reactionMe = 0xFF8B6447;
  return t;
}

Theme Config::getLightPreset() {
  Theme t;
  t.name = "Light Mode";
  t.background = 0xFFFFFFFF;
  t.backgroundDark = 0xFFF5F3F2;
  t.backgroundLight = 0xFFE5E2E0;
  t.primary = 0xFFF26558;
  t.text = 0xFF070606;
  t.textMuted = 0xFF58504E;
  t.success = 0xFF6DB143;
  t.error = 0xFF4D47F0;
  t.embed = 0xFFFBFBFB;
  t.embedMedia = 0xFFF5F3F2;
  t.reaction = 0xFFE0E2E5;
  t.reactionMe = 0xFFFAEAED;
  return t;
}

const Theme &Config::getTheme() const {
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

  PS_EncryptDecryptAes(padded_size, data.data(), data.data(),
                       PS_ALGORITHM_CTR_ENC, PS_KEYSLOT_0D, iv);
}

void Config::load() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  accounts.clear();
  struct stat st = {0};
  if (stat(CONFIG_DIR_PATH, &st) == -1) {
    mkdir(CONFIG_DIR_PATH, 0700);
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
          if (obj.IsObject() && obj.HasMember("token") &&
              obj["token"].IsString()) {
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
  } else if (currentAccountIndex < 0 ||
             currentAccountIndex >= (int)accounts.size()) {
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
      if (doc.HasMember("typing_indicator") &&
          doc["typing_indicator"].IsBool()) {
        typingIndicatorEnabled = doc["typing_indicator"].GetBool();
      }
      if (doc.HasMember("file_logging") && doc["file_logging"].IsBool()) {
        fileLoggingEnabled = doc["file_logging"].GetBool();
      }
    }
  }

  Core::I18n::getInstance().loadLanguage(language.empty() ? "en" : language);

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
  writer.EndObject();

  std::string settingsPath = std::string(CONFIG_DIR_PATH) + "/settings.json";
  Utils::File::writeFile(settingsPath, s.GetString());
}

void Config::setFileLoggingEnabled(bool enabled) {
  fileLoggingEnabled = enabled;
  Logger::setFileLoggingEnabled(enabled);
  saveSettings();
}

std::string Config::getToken() const {
  std::lock_guard<std::recursive_mutex> lock(
      const_cast<std::recursive_mutex &>(mutex));
  if (currentAccountIndex >= 0 && currentAccountIndex < (int)accounts.size()) {
    return accounts[currentAccountIndex].token;
  }
  return "";
}

void Config::setToken(const std::string &newToken) {
  addAccount("New Account", newToken);
}

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
        if (currentAccountIndex >= (int)accounts.size())
          currentAccountIndex = (int)accounts.size() - 1;
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
  std::string themePath = std::string(CONFIG_DIR_PATH) + "/theme.json";
  std::vector<char> buffer = Utils::File::readFile(themePath);
  if (buffer.empty())
    return;

  rapidjson::Document doc;
  doc.Parse(buffer.data());

  if (!doc.HasParseError() && doc.IsObject()) {
    if (doc.HasMember("name") && doc["name"].IsString())
      customTheme.name = doc["name"].GetString();

    auto loadCol = [&](const char *key, u32 &target) {
      if (doc.HasMember(key)) {
        if (doc[key].IsString()) {
          target = Utils::Color::hexToColor(doc[key].GetString());
        } else if (doc[key].IsUint()) {
          target = doc[key].GetUint();
        }
      }
    };

    loadCol("background", customTheme.background);
    loadCol("backgroundDark", customTheme.backgroundDark);
    loadCol("backgroundLight", customTheme.backgroundLight);
    loadCol("primary", customTheme.primary);
    loadCol("text", customTheme.text);
    loadCol("textMuted", customTheme.textMuted);
    loadCol("success", customTheme.success);
    loadCol("error", customTheme.error);
    loadCol("embed", customTheme.embed);
    loadCol("embedMedia", customTheme.embedMedia);
    loadCol("reaction", customTheme.reaction);
    loadCol("reactionMe", customTheme.reactionMe);
  }
}

void Config::saveTheme() {
  rapidjson::StringBuffer s;
  rapidjson::Writer<rapidjson::StringBuffer> writer(s);

  writer.StartObject();
  writer.Key("name");
  writer.String(customTheme.name.c_str());

  auto saveCol = [&](const char *key, u32 color) {
    writer.Key(key);
    writer.String(Utils::Color::colorToHex(color).c_str());
  };

  saveCol("background", customTheme.background);
  saveCol("backgroundDark", customTheme.backgroundDark);
  saveCol("backgroundLight", customTheme.backgroundLight);
  saveCol("primary", customTheme.primary);
  saveCol("text", customTheme.text);
  saveCol("textMuted", customTheme.textMuted);
  saveCol("success", customTheme.success);
  saveCol("error", customTheme.error);
  saveCol("embed", customTheme.embed);
  saveCol("embedMedia", customTheme.embedMedia);
  saveCol("reaction", customTheme.reaction);
  saveCol("reactionMe", customTheme.reactionMe);

  writer.EndObject();

  std::string themePath = std::string(CONFIG_DIR_PATH) + "/theme.json";
  Utils::File::writeFile(themePath, s.GetString());
}
