#pragma once
#include <algorithm>
#include <cmath>
#include <string_view>
#include "diagnostics.hpp"
namespace wingflight {
inline bool timedFallClip(std::string_view name) {
    return name=="MT_JumpFall" || name=="MT_JumpFall.hkx" || name=="MT_JumpFallRight.hkx"
        || name=="MT_JumpFallBack" || name=="MT_JumpFallLeft.hkx"
        || name=="Levitate_JumpFall" || name=="Levitate_JumpFall.hkx";
}
// Hold the native falling pose before its end-relative landing trigger. Only the
// selected player's held glide or owned NPC descent calls this; do not rewind or loop a
// single-play animation, edit shared trigger data, or intercept physical landings.
inline float glideClipStep(float dt,float time,float duration,float speed,float eventOffset) {
    if (!std::isfinite(dt)||dt<=0 || !std::isfinite(time)||time<0
        || !std::isfinite(duration)||duration<=0 || !std::isfinite(speed)||speed<=0) {
        if(!std::isfinite(dt)||dt<0||!std::isfinite(time)||time<0||!std::isfinite(duration)||duration<=0||!std::isfinite(speed)||speed<=0)
            diagnostic::report("held-glide.clip-time-invalid","retain native clip step",nullptr,{},time,duration);
        return dt;}
    const float stop=duration+eventOffset-0.15f;
    if(stop<=0){diagnostic::report("held-glide.stop-time-invalid","retain native clip step",nullptr,{},stop);return dt;}
    if(time>=stop) return 0;
    return std::min(dt,(stop-time)/std::max(speed,1.0f));
}
}
