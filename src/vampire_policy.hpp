#pragma once
#include "lower_body_policy.hpp"
#include "wing_blend.hpp"
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
inline bool jumpRequest(std::string_view e){
    return e=="JumpStart"||e=="JumpUp"||e=="JumpDown"||e=="JumpFall"||e=="JumpLand"||e=="JumpLandDirectional"
        ||e=="JumpStandingStart"||e=="JumpDirectionalStart"||e=="JumpFallDirectional"||e=="JumpFallLevitation"
        ||e=="JumpFallDirectionalLevitation"||e=="JumpLevitationStart"||e=="JumpLevitationDirectionalStart"||e=="JumpLevetationLand";
}
inline float safePhase(float p){return std::isfinite(p)?std::clamp(p,0.0f,1.0f):0;}
inline bool largeSprintLoop(bool nativeProvider,bool fullFlight,bool highSpeed){return nativeProvider&&fullFlight&&highSpeed;}
inline WaveWeights nativeWeights(float flap,float ownership){return waveContributions(std::clamp(flap,0.0f,1.0f),0,std::clamp(ownership,0.0f,1.0f));}
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
