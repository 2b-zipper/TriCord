#include "network/http_client.h"
#include "config.h"
#include "log.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <utils/base64_utils.h>
#include <utils/message_utils.h>

namespace Network {

HttpClient::HttpClient()
    : curl(nullptr), share(nullptr), timeout(HTTP_TIMEOUT_SECONDS),
      verifySSL(true) {
  curl = curl_easy_init();
  if (!curl) {
    Logger::log("[HTTP] Failed to initialize curl");
    return;
  }

  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);

  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  const char *caPath = "romfs:/cacert-2025-12-02.pem";
  curl_easy_setopt(curl, CURLOPT_CAINFO, caPath);

  defaultHeaders["User-Agent"] =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/143.0.0.0 Safari/537.36";

  updateSuperProperties();

  defaultHeaders["X-Debug-Options"] = "bugReporterEnabled";
  defaultHeaders["Content-Type"] = "application/json";
  defaultHeaders["Accept"] = "*/*";

  std::string lang = Config::getInstance().getLanguage();
  if (lang == "ja") {
    defaultHeaders["X-Discord-Locale"] = "ja";
    defaultHeaders["Accept-Language"] = "ja,en-US;q=0.9,en;q=0.8";
  } else {
    defaultHeaders["X-Discord-Locale"] = "en-US";
    defaultHeaders["Accept-Language"] = "en-US,en;q=0.9";
  }
}

static std::string generateRandomId(size_t len) {
  static const char charset[] = "0123456789abcdef";
  std::string result;
  result.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    result += charset[rand() % 16];
  }
  return result;
}

static std::string generateUUID() {
  return generateRandomId(8) + "-" + generateRandomId(4) + "-" +
         generateRandomId(4) + "-" + generateRandomId(4) + "-" +
         generateRandomId(12);
}

void HttpClient::updateSuperProperties() {
  rapidjson::StringBuffer s;
  rapidjson::Writer<rapidjson::StringBuffer> writer(s);

  std::string lang = Config::getInstance().getLanguage();
  std::string locale = (lang == "ja") ? "ja-JP" : "en-US";

  writer.StartObject();
  writer.Key("os");
  writer.String("Nintendo 3DS");
  writer.Key("browser");
  writer.String("TriCord");
  writer.Key("device");
  writer.String("Nintendo 3DS");
  writer.Key("system_locale");
  writer.String(locale.c_str());
  writer.Key("has_client_mods");
  writer.Bool(false);
  writer.Key("browser_user_agent");
  writer.String(defaultHeaders["User-Agent"].c_str());
  writer.Key("browser_version");
  writer.String("143.0.0.0");
  writer.Key("os_version");
  writer.String("1.0.0");
  writer.Key("referrer");
  writer.String("");
  writer.Key("referring_domain");
  writer.String("");
  writer.Key("referrer_current");
  writer.String("");
  writer.Key("referring_domain_current");
  writer.String("");
  writer.Key("release_channel");
  writer.String("stable");
  writer.Key("client_build_number");
  writer.Int(486827);
  writer.Key("client_event_source");
  writer.Null();
  writer.Key("client_launch_id");
  writer.String(generateUUID().c_str());
  writer.Key("launch_signature");
  writer.String(generateUUID().c_str());
  writer.Key("client_heartbeat_session_id");
  writer.String(generateUUID().c_str());
  writer.Key("client_app_state");
  writer.String("focused");
  writer.EndObject();

  std::string json = s.GetString();
  defaultHeaders["X-Super-Properties"] =
      Utils::Base64::encode((const unsigned char *)json.c_str(), json.length());
}

HttpClient::~HttpClient() {
  if (curl) {
    curl_easy_cleanup(curl);
    curl = nullptr;
  }
}

void HttpClient::setHeader(const std::string &key, const std::string &value) {
  defaultHeaders[key] = value;
}

void HttpClient::removeHeader(const std::string &key) {
  defaultHeaders.erase(key);
}

void HttpClient::setAuthToken(const std::string &token) {
  authToken = token;
  if (!token.empty()) {
    defaultHeaders["Authorization"] = token;
  } else {
    defaultHeaders.erase("Authorization");
  }
}

void HttpClient::setTimeout(long seconds) {
  timeout = seconds;
  if (curl)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
}

void HttpClient::setVerifySSL(bool verify) {
  verifySSL = verify;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
  }
}

void HttpClient::setShareHandle(CURLSH *handle) {
  share = handle;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_SHARE, share);
  }
}

void HttpClient::clearHeaders() {
  defaultHeaders.clear();
  defaultHeaders["User-Agent"] = APP_USER_AGENT;
  defaultHeaders["Content-Type"] = "application/json";
  defaultHeaders["Accept"] = "application/json";
}

size_t HttpClient::writeCallback(void *contents, size_t size, size_t nmemb,
                                 void *userp) {
  size_t realsize = size * nmemb;
  std::string *str = static_cast<std::string *>(userp);
  str->append(static_cast<char *>(contents), realsize);
  return realsize;
}

size_t HttpClient::headerCallback(char *buffer, size_t size, size_t nitems,
                                  void *userdata) {
  size_t realsize = size * nitems;
  auto *headers = static_cast<std::map<std::string, std::string> *>(userdata);

  std::string header(buffer, realsize);
  size_t colonPos = header.find(':');
  if (colonPos != std::string::npos) {
    std::string key = header.substr(0, colonPos);
    std::string value = header.substr(colonPos + 1);

    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
      value.erase(0, 1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
      value.pop_back();
    }

    (*headers)[key] = value;
  }

  return realsize;
}

void HttpClient::setupHeaders(
    struct curl_slist **headers,
    const std::map<std::string, std::string> &extraHeaders) {

  *headers = nullptr;

  for (const auto &pair : defaultHeaders) {
    if (extraHeaders.find(pair.first) == extraHeaders.end()) {
      std::string headerStr = pair.first + ": " + pair.second;
      *headers = curl_slist_append(*headers, headerStr.c_str());
    }
  }

  for (const auto &pair : extraHeaders) {
    std::string headerStr = pair.first + ": " + pair.second;
    *headers = curl_slist_append(*headers, headerStr.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
}

HttpResponse HttpClient::performRequest(
    const std::string &url, const std::string &method, const std::string &body,
    const std::map<std::string, std::string> &extraHeaders) {
  HttpResponse response;
  response.success = false;
  response.statusCode = 0;

  if (!curl) {
    response.error = "CURL not initialized";
    return response;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

  curl_easy_setopt(curl, CURLOPT_HTTPGET, 0L);
  curl_easy_setopt(curl, CURLOPT_POST, 0L);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.length());
  } else if (method == "PATCH") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.length());
  } else if (method == "DELETE") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
  } else {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  }

  struct curl_slist *headerList = nullptr;
  setupHeaders(&headerList, extraHeaders);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

  CURLcode res = curl_easy_perform(curl);

  if (headerList) {
    curl_slist_free_all(headerList);
  }

  if (res != CURLE_OK) {
    response.error = curl_easy_strerror(res);

    return response;
  }

  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
  response.success = (response.statusCode >= 200 && response.statusCode < 300);

  return response;
}

HttpResponse
HttpClient::get(const std::string &url,
                const std::map<std::string, std::string> &extraHeaders) {
  return performRequest(url, "GET", "", extraHeaders);
}

HttpResponse
HttpClient::post(const std::string &url, const std::string &body,
                 const std::map<std::string, std::string> &extraHeaders) {
  return performRequest(url, "POST", body, extraHeaders);
}

HttpResponse
HttpClient::patch(const std::string &url, const std::string &body,
                  const std::map<std::string, std::string> &extraHeaders) {
  return performRequest(url, "PATCH", body, extraHeaders);
}

HttpResponse
HttpClient::del(const std::string &url,
                const std::map<std::string, std::string> &extraHeaders) {
  return performRequest(url, "DELETE", "", extraHeaders);
}

} // namespace Network
