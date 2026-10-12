#pragma once
#include <algorithm>
#include <cmath>
#include "diagnostics.hpp"

namespace wingflight {
// An altitude reference schedules real wingbeats; it never prescribes a bob
// curve, sets position, brakes the apex, or cancels downward momentum.
class AutoHover {
public:
    void reset(){*this={};}
    void observe(float height){
        if(!known_){base_=height;known_=true;}
        height_=height;
        if(followManual_)base_=std::max(base_,height);
    }
    void manualClimb(){followManual_=true;}
    void automaticDownstroke(){followManual_=false;}
    void descend(float height){base_=height;followManual_=false;}
    float base() const{return base_;}
    float offset() const{return height_-base_;}
    // Forecast the SAME altitude request rule without changing the reference or
    // physical velocity. Only the recovery animation uses this time estimate.
    float requestDelay(float velocity,float gravity,float lead,float downSeconds,float impulse) const {
        if(!known_||!std::isfinite(height_)||!std::isfinite(base_)||!std::isfinite(velocity)
            ||!std::isfinite(gravity)||gravity<=0||!std::isfinite(lead)||lead<0
            ||!std::isfinite(downSeconds)||downSeconds<=0||!std::isfinite(impulse)||impulse<=0){
            if(known_)diagnostic::report("hover.prediction-input-invalid","retain unknown recovery estimate",this,
                !std::isfinite(height_)?"height":!std::isfinite(base_)?"reference height":!std::isfinite(velocity)?"velocity":
                !std::isfinite(gravity)||gravity<=0?"gravity":!std::isfinite(lead)||lead<0?"raising lead":
                !std::isfinite(downSeconds)||downSeconds<=0?"downstroke seconds":"impulse");return -1;}
        if(due(velocity,gravity,lead,downSeconds,impulse))return 0;
        auto futureDue=[&](float seconds){
            AutoHover future=*this;
            future.height_=height_+velocity*seconds-0.5f*gravity*seconds*seconds;
            return future.due(velocity-gravity*seconds,gravity,lead,downSeconds,impulse);
        };
        // Beyond the descending reference crossing the request is certainly due.
        float low=0,high=2.0f*std::max(0.0f,velocity/gravity)
            +std::sqrt(2.0f*std::max(0.0f,offset())/gravity)+lead+downSeconds+1;
        if(!std::isfinite(high)||!futureDue(high)){
            diagnostic::report("hover.prediction-bound-invalid","retain unknown recovery estimate",this,{},high);return -1;}
        for(int i=0;i<20;++i){const float middle=(low+high)*0.5f;
            if(futureDue(middle))high=middle;else low=middle;}
        return high;
    }
    bool due(float velocity,float gravity,float lead,float downSeconds,float impulse) const {
        if(!known_||!std::isfinite(velocity)||!std::isfinite(gravity)||gravity<=0
            ||!std::isfinite(lead)||lead<0||!std::isfinite(downSeconds)||downSeconds<=0
            ||!std::isfinite(impulse)||impulse<=0){
            if(known_)diagnostic::report("hover.request-input-invalid","do not request automatic beat",this,
                !std::isfinite(velocity)?"velocity":!std::isfinite(gravity)||gravity<=0?"gravity":
                !std::isfinite(lead)||lead<0?"raising lead":!std::isfinite(downSeconds)||downSeconds<=0?"downstroke seconds":"impulse");return false;}
        // Include the natural fall during raising and the time needed for the
        // downstroke to absorb that fall. This avoids waiting until too low.
        const float downVelocity=velocity-gravity*lead;
        if(downVelocity>=0)return false;
        float bottom=height_+velocity*lead-0.5f*gravity*lead*lead;
        const float braking=impulse/downSeconds-gravity;
        const float seconds=braking>0?std::min(-downVelocity/braking,downSeconds):downSeconds;
        bottom+=downVelocity*seconds+0.5f*braking*seconds*seconds;
        return bottom<=base_;
    }
private:
    bool known_{},followManual_{};
    float base_{},height_{};
};
}
