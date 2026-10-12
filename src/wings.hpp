#pragma once
#include "config.hpp"
#include "wing_sound.hpp"
#include "wing_runtime.hpp"
#include "ground_wing_recovery.hpp"
#include "logical_wings.hpp"
#include <RE/Skyrim.h>
#include <unordered_map>

namespace wingflight {
// All three pointers are borrowed for the current update, not retained owners.
struct WingMatch { const WingProfile* profile{}; RE::ActiveEffect* effect{}; RE::Actor* owner{}; };
struct WingPlayback {
    bool found{}, active{}, gliding{},spreadActive{};
    float spreadWeight{},graphSpreadWeight{};
    float spreadPhase{},spreadFrequency{};
    float phase{}, frequency{}, duration{3.166667f};
    float flapWeight{}, glideWeight{}, graphWeight{-1}, otherWeight{}, graphGlideWeight{-1}, glidePhase{}, glideFrequency{};
    std::string activeSequence;
};
class Wings {
public:
    bool initialize(float amplitude);
    void advanceVisual(float delta);
    void resolve(const Configuration& cfg);
    WingMatch find(RE::Actor* actor,bool ownedFlight=false) const;
    bool send(WingMatch match, const std::string& event) const;
    WingPlayback prepareVisual(WingMatch match);
    void prepareContinuousAnimation(WingMatch match,bool active,bool highSpeed,float fastMultiplier,float dt=0);
    void setFlapRate(float rate);
    bool setBlend(WingMatch match, float flapWeight, float ownership=1,float fast=0) const;
    bool parkInvisibleFlap();
    bool parkInvisibleGlide();
    void updateGlideRate(float multiplier,float spreadMultiplier=1);
    void updatePowerPitch(float degrees);
    void updatePowerSound(WingMatch match, const WingPlayback& playback);
    void clearVisual(bool smooth=false);
    void updateGroundRecovery(WingMatch match,float dt,bool grounded,bool fullFlight);
private:
    bool nativeActive_{},artReady_{};
    bool logicalActive_{};
    LogicalWings logical_;
    WingProfile vampireProfile_;
    WingRuntime runtime_;
    std::unordered_map<const RE::ModelReferenceEffect*,GroundWingRecovery> groundRecovery_;
    std::unordered_map<RE::EffectSetting*, const WingProfile*> effects_;
    std::unordered_map<const WingProfile*, RE::BGSSoundDescriptorForm*> sounds_;
    std::unordered_map<const WingProfile*, RE::BGSSoundDescriptorForm*> fastSounds_;
    std::unordered_map<const WingProfile*, RE::BGSSoundDescriptorForm*> spreadSounds_;
    WingStrokeSound strokeSound_;
    WingStrokeSound fastSound_;
    WingFlutterSound flutterSound_;
};
float continuousWingClipRate(const RE::hkbClipGenerator* clip,const RE::hkbContext& context);
void observeContinuousWingClip(const RE::hkbClipGenerator* clip,float before,float dt,float rate);
}
