#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <curl/curl.h>
#include <map>
#include <string>

namespace Network {

struct HttpResponse {
	long statusCode = 0;
	std::string body;
	std::string date;
	std::string retryAfter;
	bool success = false;
	std::string error;
};

struct HttpRequest {
	std::string url;
	std::string method;
	std::string body;
	std::map<std::string, std::string> headers;
	long timeoutSeconds;
};

void initHttpDefaults();
curl_slist *prepareTransfer(CURL *easy, const HttpRequest &request, HttpResponse &response);

class HttpClient {
  public:
	HttpResponse get(const std::string &url, const std::map<std::string, std::string> &extraHeaders = {});
	HttpResponse post(const std::string &url, const std::string &body,
	                  const std::map<std::string, std::string> &extraHeaders = {});
	HttpResponse patch(const std::string &url, const std::string &body,
	                   const std::map<std::string, std::string> &extraHeaders = {});
	HttpResponse put(const std::string &url, const std::string &body = "",
	                 const std::map<std::string, std::string> &extraHeaders = {});
	HttpResponse del(const std::string &url, const std::map<std::string, std::string> &extraHeaders = {});

	void setAuthToken(const std::string &token) { authToken = token; }
	void setTimeout(long seconds) { timeout = seconds; }

  private:
	HttpResponse perform(const std::string &url, const std::string &method, const std::string &body,
	                     const std::map<std::string, std::string> &extraHeaders);

	std::string authToken;
	long timeout = 0;
};

} // namespace Network

#endif // HTTP_CLIENT_H
