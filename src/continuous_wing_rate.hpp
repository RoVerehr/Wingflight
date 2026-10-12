#pragma once
#include <algorithm>
#include <cmath>
#include <string_view>

namespace wingflight {
inline bool continuousWingVisualActive(bool fullFlight,bool glideOnly,bool physicalSupport){
    return !physicalSupport&&(fullFlight||glideOnly);
}
class ContinuousWingCycleRequest {
public:
    bool update(float dt,bool active,int state){
        if(!active){cooldown_=0;return false;}
        if(std::isfinite(dt)&&dt>0)cooldown_=std::max(0.0f,cooldown_-std::min(dt,0.1f));
        if(state==2){cooldown_=0;return false;}
        if(state<0||state>4||cooldown_>0)return false;
        cooldown_=0.15f;return true;
    }
private:float cooldown_{};
};
inline float continuousWingRate(bool active,bool highSpeed,float multiplier){
    if(!active)return 0;
    if(!std::isfinite(multiplier))multiplier=1;
    return 4.0f*(highSpeed?std::clamp(multiplier,0.25f,4.0f):1.0f);
}
inline bool continuousWingClip(std::string_view name,bool looping){
    return looping&&name.size()==6&&name.starts_with("Clip0")&&name[5]>='1'&&name[5]<='5';
}
// Apply the rate only while the owned wing clip advances. Restore the provider
// value before returning, except when another hook has supplied its own value.
template<class Clip> class ScopedContinuousWingRate {
public:
    ScopedContinuousWingRate(Clip* clip,float rate):clip_(rate>0&&std::isfinite(rate)?clip:nullptr),rate_(rate){
        if(clip_){previous_=clip_->playbackSpeed;clip_->playbackSpeed=rate_;}
    }
    ~ScopedContinuousWingRate(){if(clip_&&clip_->playbackSpeed==rate_)clip_->playbackSpeed=previous_;}
    ScopedContinuousWingRate(const ScopedContinuousWingRate&)=delete;
    ScopedContinuousWingRate& operator=(const ScopedContinuousWingRate&)=delete;
private:Clip* clip_{};float previous_{},rate_{};
};
}
