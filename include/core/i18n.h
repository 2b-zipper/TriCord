#pragma once

#include <map>
#include <string>

namespace Core {

class I18n {
  public:
	static I18n &getInstance() {
		static I18n instance;
		return instance;
	}

	void init();
	bool loadLanguage(const std::string &langCode);
	std::string get(const std::string &key) const;

	static std::string format(const std::string &fmt, const std::string &arg0, const std::string &arg1 = "");

	std::string getCurrentLanguage() const { return currentLang; }

  private:
	I18n() = default;
	~I18n() = default;
	I18n(const I18n &) = delete;
	I18n &operator=(const I18n &) = delete;

	bool loadLanguageFile(const std::string &langCode, std::map<std::string, std::string> &outMap);

	std::string currentLang;
	std::map<std::string, std::string> strings;
	std::map<std::string, std::string> fallbackStrings;
};

} // namespace Core

inline std::string TR(const std::string &key) { return Core::I18n::getInstance().get(key); }
