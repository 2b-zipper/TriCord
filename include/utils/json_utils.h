#pragma once
#include <cstdint>
#include <rapidjson/document.h>
#include <string>

namespace Utils {
namespace Json {

std::string getString(const rapidjson::Value &value, const char *key);

int getInt(const rapidjson::Value &value, const char *key,
           int defaultValue = 0);

bool getBool(const rapidjson::Value &value, const char *key,
             bool defaultValue = false);

uint64_t getUint64(const rapidjson::Value &value, const char *key,
                   uint64_t defaultValue = 0);

int64_t getInt64(const rapidjson::Value &value, const char *key,
                 int64_t defaultValue = 0);

} // namespace Json
} // namespace Utils
