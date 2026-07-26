#include "core/i18n.h"
#include "log.h"
#include <cstdio>

#include "utils/file_utils.h"

#include <rapidjson/document.h>
#include <vector>

namespace Core {

void I18n::init() { loadLanguage("en_US"); }

bool I18n::loadLanguageFile(const std::string &langCode, std::map<std::string, std::string> &outMap) {
	std::string path = "romfs:/lang/" + langCode + ".json";
	std::vector<char> buffer = Utils::File::readFile(path);
	if (buffer.empty()) {
		Logger::log("Failed to open language file: %s", path.c_str());
		return false;
	}

	rapidjson::Document doc;
	doc.Parse(buffer.data());

	if (doc.HasParseError() || !doc.IsObject()) {
		Logger::log("Failed to parse language file: %s", path.c_str());
		return false;
	}

	outMap.clear();
	for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
		if (it->name.IsString() && it->value.IsString()) {
			outMap[it->name.GetString()] = it->value.GetString();
		}
	}
	return true;
}

bool I18n::loadLanguage(const std::string &langCode) {
	if (fallbackStrings.empty() && langCode != "en_US") {
		loadLanguageFile("en_US", fallbackStrings);
	}

	if (!loadLanguageFile(langCode, strings)) {
		return false;
	}

	if (langCode == "en_US") {
		fallbackStrings = strings;
	}

	currentLang = langCode;
	Logger::log("Loaded language: %s (%zu strings)", langCode.c_str(), strings.size());
	return true;
}

std::string I18n::get(const std::string &key) const {
	auto it = strings.find(key);
	if (it != strings.end()) {
		return it->second;
	}
	auto fbIt = fallbackStrings.find(key);
	if (fbIt != fallbackStrings.end()) {
		return fbIt->second;
	}
	return key;
}

std::string I18n::format(const std::string &fmt, const std::string &arg0, const std::string &arg1) {
	std::string res = fmt;
	size_t pos0 = res.find("{0}");
	if (pos0 != std::string::npos) {
		res.replace(pos0, 3, arg0);
	}
	size_t pos1 = res.find("{1}");
	if (pos1 != std::string::npos) {
		res.replace(pos1, 3, arg1);
	}
	return res;
}

} // namespace Core
