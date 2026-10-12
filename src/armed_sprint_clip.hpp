#pragma once
#include <RE/Skyrim.h>
namespace wingflight {
using ArmedIdleGate=bool(*)(RE::PlayerCharacter*);
void initializeArmedSprintIdle(ArmedIdleGate gate);
void prepareArmedSprintIdle(RE::PlayerCharacter* player,bool fullFlight);
void beforeArmedSprintActivate(RE::hkbClipGenerator* clip,const RE::hkbContext& context);
void afterArmedSprintActivate(RE::hkbClipGenerator* clip,const RE::hkbContext& context);
void afterArmedSprintDeactivate(RE::hkbClipGenerator* clip,const RE::hkbContext& context);
void updateArmedSprintIdle(RE::hkbClipGenerator* clip,const RE::hkbContext& context);
}
