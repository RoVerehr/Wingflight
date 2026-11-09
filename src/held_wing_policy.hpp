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
    struct Result {bool pending{},sendFold{},sendSneak{};};
    void begin(){active_=true;age_=retry_=0;attempts_=0;}
    Result update(float dt,int state,bool sneakCleanup=false,bool restoreSneak=false){
        if(!active_)return {};
        if(std::isfinite(dt)&&dt>0){dt=std::min(dt,.1f);age_+=dt;retry_=std::max(0.f,retry_-dt);}
        if(restoreSneak){
            if(state==2){active_=false;return {false,false,true};}
            if(state==12||state==14||age_>=1.f){active_=false;return {};}
            return {true,false};
        }
        if(state==2||(state!=6&&state!=8&&state!=10&&state!=-1&&!(sneakCleanup&&(state==12||state==14||state==16)))||age_>=1.f){active_=false;return {};}
        if((state==8||(sneakCleanup&&state==14))&&retry_<=0&&attempts_<4){++attempts_;retry_=.15f;return {true,true};}
        return {true,false};
    }
private:bool active_{};float age_{},retry_{};unsigned attempts_{};
};
inline bool sneakWingManaged(bool enabled,bool sneaking,bool airborne,bool fullFlight,bool glide,bool swimming,bool jumpPending,bool sneakPending=false){
    // Native input is accepted before IsSneaking changes. Prime the same gate
    // before that action can dispatch the wing's opening event.
    return enabled&&(sneaking||sneakPending)&&!airborne&&!fullFlight&&!glide&&!swimming&&!jumpPending;
}
inline bool sneakWingBlocks(std::string_view event){
    return event=="SneakStart"||event=="BeginWeaponDraw"||event.starts_with("BeginCast");
}
inline const char* sneakWingCommand(int state){
    if(state==14)return "SneakStop";
    if(state==20)return "SprintStop";
    if(state==8||state==26||state==32||state==36)return "MTState";
    return nullptr;
}
inline bool heldWingBlocks(std::string_view event,bool managed,bool open,bool swimming=false,bool awuEvents=true){
    if(!managed)return false;
    // Only the selected player's wing ART sees this filter. Body events,
    // effect teardown, animation completion and sound events remain native.
    const bool opening=event.starts_with("Jump")&&!event.starts_with("JumpLand")&&event.find("End")==event.npos;
    // Swim ascent shares Jump input, but must not start AWU's jump/flutter loop.
    // Never filter swim transitions, landing, completion or closing events here.
    if(swimming)return opening;
    const bool closing=event=="MTState"||event.starts_with("JumpLand")||event=="JumpSoftEnd"
        ||event=="JumpForwardEnd"||event=="JumpFastEnd"||event=="moveStart";
    const auto openingCommand=awuEvents?"JumpFall":"JumpStandingStart";
    return (opening&&!(open&&event==openingCommand))||(open&&closing);
}
// Original AWU state IDs, audited from its dragon/feather behavior graph.
// Let opening/closing clips complete naturally, then perform the pending change.
inline const char* heldWingCommand(int state,bool open,bool swimming=false,bool awuEvents=true){
    if(swimming)return nullptr; // Filter only; native swimming owns the pose.
    if(open)return state==2?(awuEvents?"JumpFall":"JumpStandingStart"):nullptr;
    // Never fold a swimming, sneaking, sprinting, casting or weapon state.
    if(state==8)return awuEvents?"MTState":"JumpLandEnd";
    return nullptr;
}
}
