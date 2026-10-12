#pragma once
#include "footstep_policy.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wingflight {
// Protect only the cosmetic handoff from an owned full-flight pose to falling.
// Physical support always wins, even on the first frame after an airborne exit.
class AirborneExit {
public:
    void reset(){controller_=0;remaining_=0;}
    void begin(bool fullFlight,bool safe,bool supported,std::uintptr_t controller){
        reset();
        if(fullFlight&&safe&&!supported&&controller){controller_=controller;remaining_=1.0f;}
    }
    bool owns(bool safe,bool supported,std::uintptr_t controller)const{
        return remaining_>0&&controller_&&safe&&!supported&&controller==controller_;
    }
    void update(float dt,bool safe,bool supported,std::uintptr_t controller){
        if(!owns(safe,supported,controller)){reset();return;}
        if(std::isfinite(dt)&&dt>0){remaining_=std::max(0.0f,remaining_-dt);if(remaining_==0)reset();}
    }
    bool suppress(bool player,bool safe,bool supported,std::uintptr_t controller,std::string_view event)const{
        // JumpLand/JumpLandEnd are state-machine messages, not cosmetic effects.
        // Do not consume them, attacks, sounds in general, or fall damage.
        return player&&owns(safe,supported,controller)&&(walkingFootEvent(event)||event=="FootLand");
    }
private:
    std::uintptr_t controller_{};
    float remaining_{};
};
}
