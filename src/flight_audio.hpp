#pragma once
#include <RE/Skyrim.h>
#include <string_view>
namespace wingflight {
// Startup-only configuration; no new sound, form, ESP or save record.
void initializeFlightAudio(bool muteBreath);
void updateFlightBreath(RE::PlayerCharacter* player,bool fullAirborneFlight);
bool registeredFootContact(std::string_view tag);
bool registeredFootSound(std::string_view editorID);
}
