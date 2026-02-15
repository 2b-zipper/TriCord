#include "utils/base64_utils.h"
#include <mbedtls/base64.h>

namespace Utils {
namespace Base64 {

std::string encode(const unsigned char *data, size_t len) {
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, data, len);

  std::vector<unsigned char> buf(olen);
  mbedtls_base64_encode(buf.data(), buf.size(), &olen, data, len);

  return std::string((char *)buf.data(), olen);
}

std::vector<unsigned char> decode(const std::string &encoded) {
  size_t olen = 0;
  mbedtls_base64_decode(nullptr, 0, &olen,
                        (const unsigned char *)encoded.c_str(),
                        encoded.length());

  std::vector<unsigned char> buf(olen);
  int ret = mbedtls_base64_decode(buf.data(), buf.size(), &olen,
                                  (const unsigned char *)encoded.c_str(),
                                  encoded.length());
  if (ret != 0) {
    return std::vector<unsigned char>();
  }

  buf.resize(olen);
  return buf;
}

} // namespace Base64
} // namespace Utils
