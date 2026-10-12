#pragma once
#include "lower_body_policy.hpp"

namespace wingflight::legs {
inline bool flightBankWanted(bool enabled,bool fullFlight,bool armed,bool armedEnabled,bool attacking,bool blocking) {
    return enabled&&fullFlight&&(!armed||armedEnabled)&&!attacking&&!blocking;
}
// Shared by armed and unarmed flight, with separate eligibility switches.
class FlightBankMotion {
public:
    void reset(){value_=0;}
    float update(float dt,float turn,bool wanted,float maximum,float seconds) {
        if(!std::isfinite(maximum)||maximum<=0){reset();return 0;}
        if(std::isfinite(dt)&&dt>0&&std::isfinite(seconds)&&seconds>0) {
            const float target=wanted&&std::isfinite(turn)&&std::abs(turn)>0.2f?std::copysign(maximum,turn):0;
            const float step=maximum*std::min(dt,0.1f)/seconds;
            value_+=std::clamp(target-value_,-step,step);
        }
        const float phase=std::clamp(std::abs(value_)/maximum,0.0f,1.0f);
        return std::copysign(maximum*phase*phase*(3-2*phase),value_);
    }
private:
    float value_{};
};
inline Quaternion bankAroundAxis(Quaternion source,float degrees,float x,float y,float z) {
    const float length=std::sqrt(x*x+y*y+z*z);
    if(!std::isfinite(length)||length<0.00001f||!std::isfinite(degrees)){
        diagnostic::report("body.overlay-axis-invalid","retain source rotation",nullptr,
            !std::isfinite(degrees)?"degrees":"axis length",!std::isfinite(degrees)?degrees:length);return source;}
    const float half=degrees*0.00872664626f,s=std::sin(half)/length;
    return multiply({x*s,y*s,z*s,std::cos(half)},source);
}
}
