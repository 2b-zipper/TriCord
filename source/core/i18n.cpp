#include "core/i18n.h"
#include "log.h"
#include <cstdio>

#include "utils/file_utils.h"

#include <rapidjson/document.h>
#include <vector>

namespace Core {

void I18n::init() { loadLanguage("en"); }

bool I18n::loadLanguage(const std::string &langCode) {
  std::string path = "romfs:/lang/" + langCode + ".json";
  std::vector<char> buffer = Utils::File::readFile(path);
  if (buffer.empty()) {
    Logger::log("Failed to open language file: %s", path.c_str());
    return false;
  }

  rapidjson::Document doc;
  doc.Parse(buffer.data());

  if (doc.HasParseError()) {
    Logger::log("Failed to parse language file: %s", path.c_str());
    return false;
  }

  if (!doc.IsObject()) {
    Logger::log("Language file is not a valid JSON object");
    return false;
  }

  strings.clear();
  currentLang = langCode;

  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    if (it->name.IsString() && it->value.IsString()) {
      strings[it->name.GetString()] = it->value.GetString();
    }
  }

  Logger::log("Loaded language: %s (%zu strings)", langCode.c_str(),
              strings.size());
  return true;
}

std::string I18n::get(const std::string &key) const {
  auto it = strings.find(key);
  if (it != strings.end()) {
    return it->second;
  }
  return key;
}

std::string I18n::format(const std::string &fmt, const std::string &arg0) {
  std::string res = fmt;
  size_t pos = res.find("{0}");
  if (pos != std::string::npos) {
    res.replace(pos, 3, arg0);
  }
  return res;
}

} // namespace Core
