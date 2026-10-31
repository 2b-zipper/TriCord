#pragma once

#include <cstddef>
#include <string>

namespace Utils {
namespace System {

const std::string &getDeviceModelName();
bool isNew3DS();
size_t heapFreeBytes();
size_t linearHeapSize();

} // namespace System
} // namespace Utils
