#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <curl/curl.h>
#include <map>
#include <string>
#include <vector>

namespace Network {

struct HttpResponse {
  long statusCode;
  std::string body;
  std::map<std::string, std::string> headers;
  bool success;
  std::string error;
};

class HttpClient {
public:
  HttpClient();
  ~HttpClient();

  // Disable copy
  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  HttpResponse get(const std::string &url,
                   const std::map<std::string, std::string> &extraHeaders = {});
  HttpResponse
  post(const std::string &url, const std::string &body,
       const std::map<std::string, std::string> &extraHeaders = {});
  HttpResponse
  patch(const std::string &url, const std::string &body,
        const std::map<std::string, std::string> &extraHeaders = {});
  HttpResponse del(const std::string &url,
                   const std::map<std::string, std::string> &extraHeaders = {});

  void setHeader(const std::string &key, const std::string &value);
  void removeHeader(const std::string &key);
  void setAuthToken(const std::string &token);
  void setTimeout(long seconds);
  void setVerifySSL(bool verify);
  void setShareHandle(CURLSH *share);

  void clearHeaders();
  void updateSuperProperties();

private:
  CURL *curl;
  CURLSH *share;
  std::map<std::string, std::string> defaultHeaders;
  std::string authToken;
  long timeout;
  bool verifySSL;

  HttpResponse
  performRequest(const std::string &url, const std::string &method,
                 const std::string &body = "",
                 const std::map<std::string, std::string> &extraHeaders = {});
  void setupCurl(const std::string &url);
  void setupHeaders(struct curl_slist **headers,
                    const std::map<std::string, std::string> &extraHeaders);

  static size_t writeCallback(void *contents, size_t size, size_t nmemb,
                              void *userp);
  static size_t headerCallback(char *buffer, size_t size, size_t nitems,
                               void *userdata);
};

} // namespace Network

#endif // HTTP_CLIENT_H
