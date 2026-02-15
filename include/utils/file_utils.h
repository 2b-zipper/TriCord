#pragma once
#include <string>
#include <vector>

namespace Utils {
namespace File {

std::vector<char> readFile(const std::string &path);
std::vector<unsigned char> readFileBinary(const std::string &path);
bool writeFile(const std::string &path, const std::vector<unsigned char> &data);
bool writeFile(const std::string &path, const std::string &data);

} // namespace File
} // namespace Utils
