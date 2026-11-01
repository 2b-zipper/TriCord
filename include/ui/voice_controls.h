#ifndef VOICE_CONTROLS_H
#define VOICE_CONTROLS_H

#include "discord/types.h"
#include <3ds.h>
#include <string>
#include <vector>

namespace UI {

namespace VoiceControls {

constexpr float BUTTON_SIZE = 30.0f;
constexpr float BUTTON_GAP = 8.0f;
constexpr float WIDTH = BUTTON_SIZE * 3.0f + BUTTON_GAP * 2.0f;

bool visible();

void draw(float x, float y);
bool handleTouch(const touchPosition &touch, float x, float y);

std::vector<Discord::VoiceParticipant> getActiveParticipants(const std::string &fallbackChannelId = "");
void drawParticipantList(float y, const std::vector<Discord::VoiceParticipant> &participants, int maxRows = 5);
bool handleParticipantTouch(const touchPosition &touch, float startY,
                            const std::vector<Discord::VoiceParticipant> &participants, int maxRows = 5);

} // namespace VoiceControls

} // namespace UI

#endif // VOICE_CONTROLS_H
