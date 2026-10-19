#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace wingflight::channel {
// Scalar poses from the actual driven stroke, not a second free-running loop.
// Fixed storage per actor; no model traversal or allocations during sampling.
class WingFlowHistory {
    struct Sample {double time{};float angle{};};
    std::array<Sample,256> samples_{};
    unsigned next_{},count_{};
    const Sample& at(unsigned index)const{return samples_[(next_+samples_.size()-count_+index)%samples_.size()];}
public:
    void clear(){next_=count_=0;}
    void push(double time,float angle){
        if(!std::isfinite(time)||!std::isfinite(angle))return;
        if(count_&&time<at(count_-1).time)clear();
        if(count_&&time==at(count_-1).time){samples_[(next_+samples_.size()-1)%samples_.size()]={time,angle};return;}
        samples_[next_]={time,angle};next_=(next_+1)%samples_.size();count_=std::min(count_+1,static_cast<unsigned>(samples_.size()));
    }
    float sample(double time,float fallback)const{
        if(!count_||!std::isfinite(time))return fallback;
        if(time<=at(0).time)return at(0).angle;
        if(time>=at(count_-1).time)return at(count_-1).angle;
        unsigned lo=0,hi=count_-1;
        while(hi-lo>1){const auto mid=(lo+hi)/2;if(at(mid).time<=time)lo=mid;else hi=mid;}
        const auto& a=at(lo);const auto& b=at(hi);
        const auto weight=static_cast<float>((time-a.time)/(b.time-a.time));
        return a.angle+(b.angle-a.angle)*weight;
    }
    float orientation(double now,float current,float position,float strength,float delay)const{
        const auto older=sample(now-std::clamp(position,0.f,1.f)*delay,current);
        return current+(older-current)*std::clamp(strength,0.f,1.f);
    }
    float jointAngle(double now,float current,float position,float parentPosition,float strength,float delay)const{
        // Child world motion already contains its parent's contribution.
        // Apply only their difference, making the distal segment truly lag.
        return orientation(now,current,position,strength,delay)-orientation(now,current,parentPosition,strength,delay);
    }
};
}
