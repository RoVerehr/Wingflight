#pragma once
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace wingflight {
void initializeAWUCompatibility(bool enabled);
void updateAWUCompatibility(RE::PlayerCharacter* player,bool ownGlide);
void resetAWUCompatibility();
bool awuEffectReady(RE::Actor* owner, RE::ActiveEffect* effect);
void awuAnimationEvent(RE::PlayerCharacter* player,const RE::BSFixedString& event);
}
