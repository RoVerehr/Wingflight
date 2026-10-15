#pragma once
#include <RE/Skyrim.h>
#include <memory>
#include "vampire_provider_policy.hpp"
namespace wingflight {
struct WingPlayback;
struct Configuration;
struct VampireWingState;
namespace vampire {enum class LevitationState;}
// Independent owner/rig/clocks for every actor; original animations are shared.
class VampireWingRuntime {
public:
    VampireWingRuntime()=default;
    ~VampireWingRuntime();
    VampireWingRuntime(const VampireWingRuntime&)=delete;
    VampireWingRuntime& operator=(const VampireWingRuntime&)=delete;
    WingPlayback prepare(RE::Actor*);
    void advance(float);
    bool blend(float flap,float ownership,float fast=0);
    void rate(float);
    void loopRate(float fast,float spread);
    void pitch(float);
    bool parkPower();
    bool parkFast();
    void release(bool smooth=false);
    void pause(bool);
private:std::shared_ptr<VampireWingState> state_;
};
// Call outside animation graph callbacks; the result can be cached per actor.
bool inspectVampireBody(const RE::Actor*);
// Read the active native stance machine, not its writable isLevitating variable.
vampire::LevitationState inspectVampireLevitation(const RE::Actor*);
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
void applyVampireWings(RE::NiAVObject*,RE::NiUpdateData* data=nullptr,bool postEvaluation=false);
bool installVampireScene();
bool vampireSceneReady();
bool nativeVampireJumpEvent(const char*);
void pauseVampireWingOverlay(bool);
}
