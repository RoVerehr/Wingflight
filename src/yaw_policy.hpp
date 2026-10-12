#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
#include "diagnostics.hpp"
namespace wingflight {
enum class YawResult:std::uint8_t { OK,NotOwner,MustKeep,AlreadyGiven,AlreadyTaken,BadThread };
inline bool wantsFlightFacing(bool enabled,bool fullFlight,float forward,float horizontal,
    bool armed,bool attacking,bool blocking) {
    return enabled&&fullFlight&&forward>0.05f&&horizontal>0.1f
        &&(!armed || (!attacking&&!blocking));
}
inline float facingYaw(float current,float desired,float dt,float response) {
    if (!std::isfinite(current)||!std::isfinite(desired)||!std::isfinite(dt)||dt<=0) {
        if(!std::isfinite(current)||!std::isfinite(desired)||!std::isfinite(dt)||dt<0)
            diagnostic::report("yaw.input-nonfinite","retain current yaw",nullptr,
                !std::isfinite(current)?"current":!std::isfinite(desired)?"desired":"delta",
                !std::isfinite(current)?current:!std::isfinite(desired)?desired:dt);
        return current;}
    const float delta=std::remainder(desired-current,6.28318530718f);
    const float angle=current+delta*(1-std::exp(-response*std::min(dt,0.1f)));
    return angle-6.28318530718f*std::floor(angle/6.28318530718f);
}
// Only release resources we own; refusal never falls through to a raw yaw write.
struct YawLease {
    bool owned{};
    YawResult last{YawResult::OK};
    template<class API,class Handle> bool acquire(API& api,Handle handle) {
        if (!owned) { last=api.RequestYawControl(handle,0.0f);owned=last==YawResult::OK||last==YawResult::AlreadyGiven; }
        return owned;
    }
    template<class API,class Handle> void release(API& api,Handle handle) {
        if (!owned)return;
        last=api.ReleaseYawControl(handle);
        if(last==YawResult::OK||last==YawResult::NotOwner)owned=false;
    }
    template<class API,class Handle> bool set(API& api,Handle handle,float angle) {
        if(!owned)return false;
        last=api.SetPlayerYaw(handle,angle);
        if(last==YawResult::NotOwner)owned=false;
        return last==YawResult::OK;
    }
};
}
