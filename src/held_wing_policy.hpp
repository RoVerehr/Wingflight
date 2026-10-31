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
// Control AWU's jump/fall opening throughout native airborne motion, including
// flight exit and walking off an edge. Non-jump events still pass unchanged.
// A just-accepted ground jump primes the gate before its native opening event.
inline bool heldWingManaged(bool enabled,bool fullFlight,bool airborne,bool jumpPending=false,bool swimming=false){
    // Swimming wins over a flight that has not yet finished its entry/exit frame.
    return enabled&&(swimming||(!fullFlight&&(airborne||jumpPending)));
}
// A landing may arrive while the native opening clip is still running.
// Keep only a short cleanup lease, with the event filter already disabled.
class HeldWingLanding {
public:
    struct Result {bool pending{},sendFold{};};
    void begin(){active_=true;age_=retry_=0;attempts_=0;}
    Result update(float dt,int state){
        if(!active_)return {};
        if(std::isfinite(dt)&&dt>0){dt=std::min(dt,.1f);age_+=dt;retry_=std::max(0.f,retry_-dt);}
        if(state==2||(state!=6&&state!=8&&state!=10&&state!=-1)||age_>=1.f){active_=false;return {};}
        if(state==8&&retry_<=0&&attempts_<4){++attempts_;retry_=.15f;return {true,true};}
        return {true,false};
    }
private:bool active_{};float age_{},retry_{};unsigned attempts_{};
};
inline bool heldWingBlocks(std::string_view event,bool managed,bool open,bool swimming=false){
    if(!managed)return false;
    // Only the selected player's wing ART sees this filter. Body events,
    // effect teardown, animation completion and sound events remain native.
    const bool opening=event.starts_with("Jump")&&!event.starts_with("JumpLand")&&event.find("End")==event.npos;
    // Swim ascent shares Jump input, but must not start AWU's jump/flutter loop.
    // Never filter swim transitions, landing, completion or closing events here.
    if(swimming)return opening;
    const bool closing=event=="MTState"||event.starts_with("JumpLand")||event=="JumpSoftEnd"
        ||event=="JumpForwardEnd"||event=="JumpFastEnd"||event=="moveStart";
    return (opening&&!(open&&event=="JumpFall"))||(open&&closing);
}
// Original AWU state IDs, audited from its dragon/feather behavior graph.
// Let opening/closing clips complete naturally, then perform the pending change.
inline const char* heldWingCommand(int state,bool open,bool swimming=false){
    if(swimming)return nullptr; // Filter only; native swimming owns the pose.
    if(open)return state==2?"JumpFall":nullptr;
    // Never fold a swimming, sneaking, sprinting, casting or weapon state.
    if(state==8)return "MTState";
    return nullptr;
}
}
