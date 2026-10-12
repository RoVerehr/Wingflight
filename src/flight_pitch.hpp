#pragma once
#include "armed_bank.hpp"

namespace wingflight::legs {
// Flight velocity and the skeleton share a Z-up coordinate system. Positive
// elevation is climbing, so it REDUCES the existing forward lean.
inline float flightPathPitch(float x,float y,float z) {
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)){
        diagnostic::report("body.flight-path-nonfinite","use zero additive flight pitch",nullptr,
            !std::isfinite(x)?"x":!std::isfinite(y)?"y":"z",!std::isfinite(x)?x:!std::isfinite(y)?y:z);return 0;}
    const float horizontal=std::hypot(x,y);
    if(std::hypot(horizontal,z)<0.05f)return 0;
    return std::atan2(z,horizontal)*57.2957795131f;
}
class FlightPitchMotion {
public:
    void reset(){angle_=0;}
    float angle()const{return angle_;}
    float update(float dt,float elevation,bool wanted,float seconds) {
        const float target=wanted&&std::isfinite(elevation)?std::clamp(elevation,-90.0f,90.0f):0;
        if(std::isfinite(dt)&&dt>0&&std::isfinite(seconds)&&seconds>0) {
            angle_+=(target-angle_)*-std::expm1(-std::min(dt,0.1f)/seconds);
            if(std::abs(angle_-target)<0.0001f)angle_=target;
        }
        return angle_;
    }
private:
    float angle_{};
};
inline Quaternion pitchAroundAxis(Quaternion source,float elevation,float x,float y,float z) {
    // Positive actor-right rotation lifts the existing prone pose toward the
    // flight path. Apply this after banking, so the bank follows that path too.
    return bankAroundAxis(source,elevation,x,y,z);
}
}
