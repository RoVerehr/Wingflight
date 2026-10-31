#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>

namespace wingflight {
// A replacement proxy can report its initial support value before native
// movement has updated it. Only the owner samples this after native updates;
// animation callbacks can read the published result without advancing it.
class ControllerSupport {
public:
    void reset(bool ground=false){
        ground_=ground;pending_=false;groundSeconds_=0;groundSamples_=airSamples_=0;
    }
    void replaced(){groundSeconds_=0;groundSamples_=airSamples_=0;pending_=true;}
    bool pending() const {return pending_.load();}
    bool supported(bool raw) const {return pending_?ground_.load():raw;}
    void afterNative(bool raw,float dt){
        if(!std::isfinite(dt)||dt<=0)return;
        if(!pending_){ground_=raw;return;}
        if(!raw){
            ground_=false;groundSeconds_=0;groundSamples_=0;
            if(++airSamples_>=2)pending_=false;
        }else{
            airSamples_=0;++groundSamples_;groundSeconds_+=std::min(dt,.1f);
            if(groundSamples_>=2&&groundSeconds_>=.06f){ground_=true;pending_=false;}
        }
    }
private:
    std::atomic_bool ground_{},pending_{};
    float groundSeconds_{};
    unsigned groundSamples_{},airSamples_{};
};
}
