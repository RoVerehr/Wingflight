#pragma once
#include <algorithm>
#include <cmath>
namespace wingflight {
// The native AWU opening state ignores landing events. Wait for stable physical
// ground, then replay a wing-only idle event once the opening reaches its loop.
class GroundWingRecovery {
public:
    void reset(){grounded_=retry_=0;attempts_=0;}
    bool update(float dt,bool grounded,bool flying,bool knownJumpFlutterLoop){
        if(!std::isfinite(dt)||dt<=0)return false;
        if(!grounded||flying){reset();return false;}
        const float step=std::min(dt,0.1f);
        grounded_+=step;retry_=std::max(0.0f,retry_-step);
        if(!knownJumpFlutterLoop||grounded_<0.5f||retry_>0||attempts_>=3)return false;
        ++attempts_;retry_=0.5f;return true;
    }
private:
    float grounded_{},retry_{};
    unsigned attempts_{};
};
// Held wings never own a full flight's wings, so a full-flight exit that
// misses the native landing fold (a damaging collision drops into native
// falling) keeps the recovery above running for a few grounded seconds.
class FlightExitWingWindow {
public:
    void reset(){remaining_=0;}
    void begin(){remaining_=seconds;}
    // True while the recovery should run regardless of the held-wing gate.
    bool update(float dt,bool flying,bool grounded){
        if(flying){remaining_=0;return false;}
        const bool open=remaining_>0;
        if(open&&grounded&&std::isfinite(dt)&&dt>0)remaining_=std::max(0.0f,remaining_-std::min(dt,0.1f));
        return open;
    }
    static constexpr float seconds=3.0f; // Covers the 0.5 s debounce and all three retries.
private:
    float remaining_{};
};
}
