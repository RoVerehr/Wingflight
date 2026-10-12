#pragma once
#include <RE/Skyrim.h>
#include "vampire_provider_policy.hpp"
namespace wingflight {
struct WingPlayback;
struct Configuration;
void resolveVampireFlight(const Configuration&);
bool isNativeVampireLord(const RE::Actor*);
// Refresh outside graph/scene callbacks; body and visible wing backend are independent.
void refreshVampireContext(RE::PlayerCharacter*);
bool usesVampireLordBehavior(const RE::Actor*);
vampire::Driver vampireWingDriver();
bool nativeVampireFlightEligible(const RE::Actor*,bool ownedFlight=false);
WingPlayback prepareVampireWings(RE::Actor*);
void advanceVampireWings(float);
bool blendVampireWings(float flap,float ownership,float fast);
void rateVampireWings(float);
void loopRateVampireWings(float fast,float spread);
void pitchVampireWings(float);
bool parkVampirePower();
bool parkVampireFast();
void releaseVampireWings(bool smooth);
void applyVampireWings(RE::NiAVObject*);
bool installVampireScene();
bool vampireSceneReady();
bool nativeVampireJumpEvent(const char*);
void pauseVampireWingOverlay(bool);
}
