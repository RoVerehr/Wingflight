#pragma once
#include "lower_body_policy.hpp"
#include "wing_blend.hpp"
#include "wing_runtime_policy.hpp"
#include <algorithm>
#include <string_view>
#include <cmath>
namespace wingflight::vampire {
inline bool eligible(bool enabled,bool nativeRace,bool loaded,bool alive,bool ragdoll,bool nativeGraph,bool tenBones){
    return enabled&&nativeRace&&loaded&&alive&&!ragdoll&&nativeGraph&&tenBones;
}
// isLevitating is written by both the native levitation wrapper and its jump
// sub-state. A successful owned flight must survive the sub-state's exit.
// Missing variables still indicate an invalid graph and remain a hard failure.
inline bool stancePermitsFlight(bool readable,bool levitating,bool ownedFlight){
    return readable&&(levitating||ownedFlight);
}
inline bool followerStancePermitsFlight(bool readable,bool levitating,bool ownedFlight,bool inCombat){
    return readable&&(levitating||ownedFlight||!inCombat);
}
enum class LevitationState { unavailable,ground,entering,levitating,leaving };
// Native H2HBehavior states, checked against the original behavior fixture.
inline LevitationState levitationState(int state,bool blending=false){
    switch(state){
    case 0:return LevitationState::ground;
    case 48:return blending?LevitationState::entering:LevitationState::levitating;
    case 49:return LevitationState::leaving;
    case 50:return LevitationState::entering;
    default:return LevitationState::unavailable;
    }
}
struct LevitationTakeoff {
    struct Decision {bool request{},ready{},timedOut{};};
    bool pending{};float age{},stable{},cooldown{};
    bool preparingFollow()const{return pending||(stable>0&&stable<.5f);}
    Decision update(float dt,LevitationState state,bool maySwitch){
        dt=std::isfinite(dt)?std::clamp(dt,0.f,.1f):0.f;
        cooldown=std::max(0.f,cooldown-dt);
        if(state==LevitationState::levitating){
            pending=false;age=0;stable+=dt;
            return {false,stable>=.10f,false};
        }
        stable=0;
        if(pending){
            age+=dt;
            if(age>=3.f){pending=false;age=0;cooldown=2.f;return {false,false,true};}
        }else if(state==LevitationState::ground&&maySwitch&&cooldown<=0){
            pending=true;age=0;return {true,false,false};
        }
        return {};
    }
};
inline bool jumpRequest(std::string_view e){
    return e=="JumpStart"||e=="JumpUp"||e=="JumpDown"||e=="JumpFall"||e=="JumpLand"||e=="JumpLandDirectional"
        ||e=="JumpStandingStart"||e=="JumpDirectionalStart"||e=="JumpFallDirectional"||e=="JumpFallLevitation"
        ||e=="JumpFallDirectionalLevitation"||e=="JumpLevitationStart"||e=="JumpLevitationDirectionalStart"||e=="JumpLevetationLand";
}
inline float safePhase(float p){return std::isfinite(p)?std::clamp(p,0.0f,1.0f):0;}
inline bool largeSprintLoop(bool nativeProvider,bool fullFlight,bool highSpeed){return nativeProvider&&fullFlight&&highSpeed;}
inline WaveWeights nativeWeights(float flap,float ownership){return waveContributions(std::clamp(flap,0.0f,1.0f),0,std::clamp(ownership,0.0f,1.0f));}
struct PlaybackState {
    std::array<visual::Clock,3> clocks;
    WaveWeights desired{},displayed{};
    float delta{};
    bool released{};
    std::uint64_t requested{},applied{~0ULL};
    void advancePlayback(float dt){
        delta=std::isfinite(dt)?std::clamp(dt,0.f,.1f):0;
        for(auto& clock:clocks)clock.advance(delta);
        ++requested;
        const float mix=1-std::exp(-delta*25);
        displayed.flap+=(desired.flap-displayed.flap)*mix;
        displayed.fast+=(desired.fast-displayed.fast)*mix;
        displayed.spread+=(desired.spread-displayed.spread)*mix;
        displayed.original=std::max(0.f,1-displayed.flap-displayed.fast-displayed.spread);
    }
};
inline legs::Quaternion flutterPose(legs::Quaternion spread,legs::Quaternion idleCenter,legs::Quaternion idle,float amplitude){
    auto delta=legs::multiply({-idleCenter.x,-idleCenter.y,-idleCenter.z,idleCenter.w},idle);
    if(delta.w<0)delta={-delta.x,-delta.y,-delta.z,-delta.w};
    const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    if(length<1e-7f)return spread;
    const float half=std::atan2(length,delta.w)*std::clamp(amplitude,0.0f,3.0f);
    const float factor=std::sin(half)/length;
    return legs::multiply(spread,{delta.x*factor,delta.y*factor,delta.z*factor,std::cos(half)});
}
}
