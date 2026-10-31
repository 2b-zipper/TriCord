#include "utils/redraw.h"

#include <atomic>

namespace Utils {

namespace {
std::atomic<bool> redrawRequested{true};
}

void requestRedraw() { redrawRequested.store(true); }

bool consumeRedrawRequest() { return redrawRequested.exchange(false); }

} // namespace Utils
