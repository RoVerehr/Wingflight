#pragma once
#include <algorithm>
#include <cmath>
#include <string_view>
namespace wingflight {
class HeldWingIntent {
public:
    void reset(){age_=0;jumped_=false;}
    bool update(float dt,bool held,bool airborne,bool fullFlight,bool glide,bool suspended,float threshold){
        if(!held||fullFlight||suspended){reset();return false;}
        if(!std::isfinite(dt)||dt<=0||!std::isfinite(threshold)||threshold<=0)return false;
        jumped_|=airborne;
        age_=std::min(threshold,age_+std::min(dt,.1f));
        return glide||(jumped_&&age_>=threshold);
    }
private:float age_{};bool jumped_{};
};
inline bool heldWingBlocks(std::string_view event,bool managed,bool open){
    if(!managed)return false;
    // Only the selected player's wing ART sees this filter. Body events,
    // effect teardown, animation completion and sound events remain native.
    const bool opening=(event.starts_with("Jump")&&!event.starts_with("JumpLand")&&event.find("End")==event.npos)
        ||event=="SneakStart"||event=="SwimStart"||event=="BeginWeaponDraw"
        ||event.starts_with("BeginCast")||event.starts_with("eFlying")||event=="Flying1HAttackLeft";
    const bool closing=event=="MTState"||event.starts_with("JumpLand")||event=="JumpSoftEnd"
        ||event=="JumpForwardEnd"||event=="JumpFastEnd"||event=="SneakStop"||event=="SprintStop"||event=="SwimStop"||event=="moveStart";
    return (opening&&!(open&&event=="JumpFall"))||(open&&closing);
}
// Original AWU state IDs, audited from its dragon/feather behavior graph.
// Let opening/closing clips complete naturally, then perform the pending change.
inline const char* heldWingCommand(int state,bool open){
    if(open)return state==2?"JumpFall":nullptr;
    if(state==8||state==26||state==32||state==36)return "MTState";
    if(state==14)return "SneakStop";
    if(state==20)return "SprintStop";
    return nullptr;
}
}
