#pragma once
#include <string>
#include <vector>

namespace Utils {
namespace Base64 {

std::string encode(const unsigned char *data, size_t len);
std::vector<unsigned char> decode(const std::string &encoded);

} // namespace Base64
} // namespace Utils
