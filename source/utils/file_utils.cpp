#include "utils/file_utils.h"
#include <cstdio>
#include <vector>

namespace Utils {
namespace File {

std::vector<char> readFile(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "r");
  if (!fp)
    return {};

  fseek(fp, 0, SEEK_END);
  size_t size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  std::vector<char> buffer(size + 1);
  if (size > 0) {
    fread(buffer.data(), 1, size, fp);
  }
  buffer[size] = '\0';
  fclose(fp);
  return buffer;
}

std::vector<unsigned char> readFileBinary(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp)
    return {};

  fseek(fp, 0, SEEK_END);
  size_t size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  std::vector<unsigned char> buffer(size);
  if (size > 0) {
    fread(buffer.data(), 1, size, fp);
  }
  fclose(fp);
  return buffer;
}

bool writeFile(const std::string &path,
               const std::vector<unsigned char> &data) {
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp)
    return false;
  fwrite(data.data(), 1, data.size(), fp);
  fclose(fp);
  return true;
}

bool writeFile(const std::string &path, const std::string &data) {
  FILE *fp = fopen(path.c_str(), "w");
  if (!fp)
    return false;
  fputs(data.c_str(), fp);
  fclose(fp);
  return true;
}

} // namespace File
} // namespace Utils
