#include "utils/json_utils.h"
#include <cstdlib>

namespace Utils {
namespace Json {

namespace {
uint64_t safeStoull(const char *str, uint64_t defaultValue) {
  if (!str || *str == '\0')
    return defaultValue;
  char *endptr;
  uint64_t val = strtoull(str, &endptr, 10);
  if (endptr == str)
    return defaultValue;
  return val;
}

int64_t safeStoll(const char *str, int64_t defaultValue) {
  if (!str || *str == '\0')
    return defaultValue;
  char *endptr;
  int64_t val = strtoll(str, &endptr, 10);
  if (endptr == str)
    return defaultValue;
  return val;
}
} // namespace

std::string getString(const rapidjson::Value &value, const char *key) {
  auto it = value.FindMember(key);
  if (it != value.MemberEnd()) {
    if (it->value.IsString()) {
      return std::string(it->value.GetString(), it->value.GetStringLength());
    }
    if (it->value.IsInt64()) {
      return std::to_string(it->value.GetInt64());
    }
    if (it->value.IsInt()) {
      return std::to_string(it->value.GetInt());
    }
    if (it->value.IsUint64()) {
      return std::to_string(it->value.GetUint64());
    }
  }
  return "";
}

int getInt(const rapidjson::Value &value, const char *key, int defaultValue) {
  auto it = value.FindMember(key);
  if (it != value.MemberEnd()) {
    if (it->value.IsInt()) {
      return it->value.GetInt();
    }
    if (it->value.IsString()) {
      return std::atoi(it->value.GetString());
    }
  }
  return defaultValue;
}

bool getBool(const rapidjson::Value &value, const char *key,
             bool defaultValue) {
  auto it = value.FindMember(key);
  if (it != value.MemberEnd() && it->value.IsBool()) {
    return it->value.GetBool();
  }
  return defaultValue;
}

uint64_t getUint64(const rapidjson::Value &value, const char *key,
                   uint64_t defaultValue) {
  auto it = value.FindMember(key);
  if (it != value.MemberEnd()) {
    if (it->value.IsString()) {
      return safeStoull(it->value.GetString(), defaultValue);
    }
    if (it->value.IsUint64()) {
      return it->value.GetUint64();
    }
    if (it->value.IsInt64()) {
      return (uint64_t)it->value.GetInt64();
    }
    if (it->value.IsInt()) {
      return (uint64_t)it->value.GetInt();
    }
  }
  return defaultValue;
}

int64_t getInt64(const rapidjson::Value &value, const char *key,
                 int64_t defaultValue) {
  auto it = value.FindMember(key);
  if (it != value.MemberEnd()) {
    if (it->value.IsString()) {
      return safeStoll(it->value.GetString(), defaultValue);
    }
    if (it->value.IsInt64()) {
      return it->value.GetInt64();
    }
    if (it->value.IsInt()) {
      return (int64_t)it->value.GetInt();
    }
    if (it->value.IsUint64()) {
      return (int64_t)it->value.GetUint64();
    }
  }
  return defaultValue;
}

} // namespace Json
} // namespace Utils
