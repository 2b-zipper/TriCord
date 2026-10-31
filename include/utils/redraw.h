#pragma once

namespace Utils {

// Frames with nothing new are not drawn; anything that changes what is on
// screen outside of input or a running animation must ask for one.
void requestRedraw();
bool consumeRedrawRequest();

} // namespace Utils
