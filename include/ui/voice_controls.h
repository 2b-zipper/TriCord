#ifndef VOICE_CONTROLS_H
#define VOICE_CONTROLS_H

#include <3ds.h>

namespace UI {

namespace VoiceControls {

constexpr float BUTTON_SIZE = 30.0f;
constexpr float BUTTON_GAP = 8.0f;
constexpr float WIDTH = BUTTON_SIZE * 3.0f + BUTTON_GAP * 2.0f;

bool visible();

void draw(float x, float y);
bool handleTouch(const touchPosition &touch, float x, float y);

} // namespace VoiceControls

} // namespace UI

#endif // VOICE_CONTROLS_H
