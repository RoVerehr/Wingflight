#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace wingflight::legs {
enum class WeaponPosePhase:std::uint8_t { sheathed,drawing,drawn,sheathing };
struct WeaponLegHandoffState { bool protect{},capture{};float poseBlend{1}; };
// Weapon-drawn alone omits the draw animation and the outgoing sheath blend.
// During this handoff, blend two flight leg poses instead of letting incoming
// equip/unequip locomotion provide the source pose.
class WeaponLegHandoff {
public:
    void reset(){*this={};}
    WeaponLegHandoffState update(WeaponPosePhase phase,bool enabled,float dt,
        float poseSeconds,float recoverySeconds){
        if(!enabled){reset();return {};}
        const bool changing=phase==WeaponPosePhase::drawing||phase==WeaponPosePhase::sheathing;
        bool capture{};
        if(!initialized_){initialized_=true;previous_=phase;if(changing){active_=true;capture=true;}}
        else if(phase!=previous_){
            const bool completed=(previous_==WeaponPosePhase::drawing&&phase==WeaponPosePhase::drawn)||
                (previous_==WeaponPosePhase::sheathing&&phase==WeaponPosePhase::sheathed);
            capture=!completed;active_=true;settledAge_=0;previous_=phase;
        }
        if(capture)poseAge_=0;
        const float step=std::isfinite(dt)&&dt>0?std::min(dt,0.1f):0;
        if(active_){
            poseAge_+=step;
            if(changing)settledAge_=0;
            else settledAge_+=step;
            if(!changing&&settledAge_>=std::max(recoverySeconds,0.01f))active_=false;
        }
        const float x=std::clamp(poseAge_/std::max(poseSeconds,0.01f),0.0f,1.0f);
        return {active_,capture,x*x*(3-2*x)};
    }
private:
    bool initialized_{},active_{};
    WeaponPosePhase previous_{};
    float poseAge_{},settledAge_{};
};
}
