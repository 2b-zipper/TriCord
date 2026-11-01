#include "network/http_client.h"
#include "config.h"
#include "log.h"
#include "network/network_manager.h"
#include "network/tls.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mbedtls/ssl.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <strings.h>
#include <utils/base64_utils.h>
#include <utils/system_utils.h>
#include <vector>

namespace Network {

namespace {

constexpr const char *USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
                                   "like Gecko) Chrome/143.0.0.0 Safari/537.36";
constexpr long MAX_REDIRECTS = 5;
constexpr long KEEPALIVE_IDLE_SECONDS = 120;
constexpr long KEEPALIVE_INTERVAL_SECONDS = 60;
constexpr long DNS_CACHE_SECONDS = 600;
constexpr long CONNECTION_MAX_AGE_SECONDS = 600;
constexpr size_t LARGE_BODY = 64 * 1024;
constexpr size_t BODY_HEAP_MARGIN = 1024 * 1024;

struct DefaultHeader {
	std::string name;
	std::string line;
	bool discordOnly;
};

std::vector<DefaultHeader> defaultHeaders;

void addDefaultHeader(const std::string &name, const std::string &value, bool discordOnly = false) {
	defaultHeaders.push_back({name, name + ": " + value, discordOnly});
}

std::string generateRandomId(size_t len) {
	static const char charset[] = "0123456789abcdef";
	std::string result;
	result.reserve(len);
	for (size_t i = 0; i < len; ++i) {
		result += charset[rand() % 16];
	}
	return result;
}

std::string generateUUID() {
	return generateRandomId(8) + "-" + generateRandomId(4) + "-" + generateRandomId(4) + "-" + generateRandomId(4) +
	       "-" + generateRandomId(12);
}

std::string buildSuperProperties(const std::string &lang) {
	rapidjson::StringBuffer s;
	rapidjson::Writer<rapidjson::StringBuffer> writer(s);

	const std::string locale = (lang == "ja") ? "ja-JP" : "en-US";

	writer.StartObject();
	writer.Key("os");
	writer.String(Utils::System::getDeviceModelName().c_str());
	writer.Key("browser");
	writer.String("TriCord");
	writer.Key("device");
	writer.String(Utils::System::getDeviceModelName().c_str());
	writer.Key("system_locale");
	writer.String(locale.c_str());
	writer.Key("has_client_mods");
	writer.Bool(false);
	writer.Key("browser_user_agent");
	writer.String(USER_AGENT);
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

	const std::string json = s.GetString();
	return Utils::Base64::encode((const unsigned char *)json.c_str(), json.length());
}

bool isDiscordUrl(const std::string &url) {
	return url.find("discord.com") != std::string::npos || url.find("discordapp.com") != std::string::npos ||
	       url.find("discordapp.net") != std::string::npos;
}

curl_slist *buildHeaderList(const HttpRequest &request) {
	const bool discord = isDiscordUrl(request.url);
	curl_slist *list = nullptr;

	for (const DefaultHeader &header : defaultHeaders) {
		if ((discord || !header.discordOnly) && request.headers.find(header.name) == request.headers.end()) {
			list = curl_slist_append(list, header.line.c_str());
		}
	}
	for (const auto &pair : request.headers) {
		list = curl_slist_append(list, (pair.first + ": " + pair.second).c_str());
	}
	return list;
}

bool fitsInHeap(size_t bytes) {
	return bytes <= LARGE_BODY || bytes + BODY_HEAP_MARGIN <= Utils::System::heapFreeBytes();
}

size_t receiveBody(void *contents, size_t size, size_t nmemb, void *userp) {
	auto *body = static_cast<std::string *>(userp);
	const size_t len = size * nmemb;
	const size_t needed = body->size() + len;

	if (needed > body->capacity() && !fitsInHeap(std::max(needed, body->capacity() * 2))) {
		Logger::log("[HTTP] Body of over %zu bytes does not fit in memory", needed);
		return 0;
	}
	body->append(static_cast<char *>(contents), len);
	return len;
}

bool headerIs(const char *line, size_t len, const char *name) {
	const size_t nameLen = strlen(name);
	return len > nameLen && line[nameLen] == ':' && strncasecmp(line, name, nameLen) == 0;
}

std::string headerValue(const char *line, size_t len, size_t nameLen) {
	size_t start = nameLen + 1;
	while (start < len && (line[start] == ' ' || line[start] == '\t')) {
		start++;
	}
	while (len > start && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
		len--;
	}
	return std::string(line + start, len - start);
}

size_t receiveHeader(char *line, size_t size, size_t nitems, void *userdata) {
	auto *response = static_cast<HttpResponse *>(userdata);
	const size_t len = size * nitems;

	if (headerIs(line, len, "Date")) {
		response->date = headerValue(line, len, 4);
	} else if (headerIs(line, len, "Retry-After")) {
		response->retryAfter = headerValue(line, len, 11);
	} else if (headerIs(line, len, "Content-Length")) {
		const size_t length = strtoul(line + 15, nullptr, 10);
		if (fitsInHeap(length)) {
			response->body.reserve(length);
		}
	}
	return len;
}

CURLcode useSharedCaChain(CURL *, void *sslConfig, void *) {
	mbedtls_ssl_conf_ca_chain(static_cast<mbedtls_ssl_config *>(sslConfig), sharedCaChain(), nullptr);
	return CURLE_OK;
}

} // namespace

void initHttpDefaults() {
	if (!defaultHeaders.empty()) {
		return;
	}
	const std::string lang = Config::getInstance().getLanguage();
	const bool japanese = lang == "ja";

	addDefaultHeader("Accept", "*/*");
	addDefaultHeader("Accept-Language", japanese ? "ja,en-US;q=0.9,en;q=0.8" : "en-US,en;q=0.9");
	addDefaultHeader("Content-Type", "application/json");
	addDefaultHeader("User-Agent", USER_AGENT);
	addDefaultHeader("X-Debug-Options", "bugReporterEnabled");
	addDefaultHeader("X-Discord-Locale", japanese ? "ja" : "en-US", true);
	addDefaultHeader("X-Super-Properties", buildSuperProperties(lang), true);
}

curl_slist *prepareTransfer(CURL *easy, const HttpRequest &request, HttpResponse &response) {
	curl_easy_reset(easy);

	curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_TIMEOUT, request.timeoutSeconds);
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(easy, CURLOPT_MAXREDIRS, MAX_REDIRECTS);
	curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, KEEPALIVE_IDLE_SECONDS);
	curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, KEEPALIVE_INTERVAL_SECONDS);
	curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, DNS_CACHE_SECONDS);
	curl_easy_setopt(easy, CURLOPT_MAXAGE_CONN, CONNECTION_MAX_AGE_SECONDS);

	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(easy, CURLOPT_CAINFO, nullptr);
	curl_easy_setopt(easy, CURLOPT_CAPATH, nullptr);
	curl_easy_setopt(easy, CURLOPT_SSL_CTX_FUNCTION, useSharedCaChain);

	const std::string &method = request.method;
	if (method == "POST") {
		curl_easy_setopt(easy, CURLOPT_POST, 1L);
	} else if (method != "GET") {
		curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
	}
	if (method == "POST" || method == "PATCH" || (method != "GET" && !request.body.empty())) {
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.c_str());
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)request.body.length());
	}

	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receiveBody);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, receiveHeader);
	curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response);

	curl_slist *headers = buildHeaderList(request);
	curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
	return headers;
}

HttpResponse HttpClient::perform(const std::string &url, const std::string &method, const std::string &body,
                                 const std::map<std::string, std::string> &extraHeaders) {
	HttpRequest request{url, method, body, extraHeaders, timeout > 0 ? timeout : HTTP_TIMEOUT_SECONDS};
	if (!authToken.empty()) {
		request.headers["Authorization"] = authToken;
	}
	return NetworkManager::getInstance().perform(std::move(request));
}

HttpResponse HttpClient::get(const std::string &url, const std::map<std::string, std::string> &extraHeaders) {
	return perform(url, "GET", "", extraHeaders);
}

HttpResponse HttpClient::post(const std::string &url, const std::string &body,
                              const std::map<std::string, std::string> &extraHeaders) {
	return perform(url, "POST", body, extraHeaders);
}

HttpResponse HttpClient::patch(const std::string &url, const std::string &body,
                               const std::map<std::string, std::string> &extraHeaders) {
	return perform(url, "PATCH", body, extraHeaders);
}

HttpResponse HttpClient::put(const std::string &url, const std::string &body,
                             const std::map<std::string, std::string> &extraHeaders) {
	return perform(url, "PUT", body, extraHeaders);
}

HttpResponse HttpClient::del(const std::string &url, const std::map<std::string, std::string> &extraHeaders) {
	return perform(url, "DELETE", "", extraHeaders);
}

} // namespace Network
