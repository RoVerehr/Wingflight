#pragma once
namespace wingflight {
enum class ExternalFlightOwner { none, framework, flyingMod };
// Read dedicated provider state, never generic IsFlying or native air state.
// Flying Mod's IsFlightEnabled() compares FlyingToggle with exactly 1.
inline ExternalFlightOwner externalFlightOwner(bool frameworkFlying,float flyingToggle){
    if(frameworkFlying)return ExternalFlightOwner::framework;
    if(flyingToggle==1.f)return ExternalFlightOwner::flyingMod;
    return ExternalFlightOwner::none;
}
inline const char* externalFlightName(ExternalFlightOwner owner){
    switch(owner){
    case ExternalFlightOwner::framework:return "Flight Framework";
    case ExternalFlightOwner::flyingMod:return "Flying Mod";
    default:return "none";
    }
}
}
