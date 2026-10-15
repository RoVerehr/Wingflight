#pragma once
#include "actor_flight_policy.hpp"

namespace wingflight::aerial {
// Skyrim world coordinates, matching the native projectile solver and DirectAt
// modifier. Only a short-lived value crosses into the animation worker.
struct SpellAimFrame {
    Vec3 point{};
    std::uint32_t frame{},spell{},enemy{};
    bool available(std::uint32_t now)const{
        return frame&&now&&spell&&enemy&&now-frame<=2&&valid(point);
    }
};
inline bool bridgeSpellAim(bool nativeTarget,std::uint32_t userInfo,bool owned,bool landing,bool paused,
    const SpellAimFrame& aim,std::uint32_t now){
    // Native DirectAt userInfo 2 is magic; bow and headtracking providers retain
    // their own targets. Never replace a target supplied by the previous hook.
    return !nativeTarget&&userInfo==2&&owned&&!landing&&!paused&&aim.available(now);
}
inline bool usableSpellAimPoint(Vec3 origin,Vec3 point){
    const auto distance=length(subtract(point,origin));
    return valid(origin)&&valid(point)&&std::isfinite(distance)&&distance>1.f&&distance<8192.f;
}
}
