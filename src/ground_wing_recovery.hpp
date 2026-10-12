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
}
