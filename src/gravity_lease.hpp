#pragma once
#include <cmath>
#include "diagnostics.hpp"

namespace wingflight {
// Temporarily own one controller scalar; preserve the original value and any
// later external update. Never change the world's gravity or another actor.
class GravityLease {
public:
    bool apply(float& value){
        if(!std::isfinite(value)){
            diagnostic::report("gravity.controller-value-nonfinite","do not acquire gravity scalar",this,{},value);return false;}
        if(!active_||value!=0){original_=value;active_=true;}
        value=0;return true;
    }
    void release(float& value){
        if(active_&&value==0)value=original_;
        active_=false;
    }
private:
    bool active_{};
    float original_{};
};
}
