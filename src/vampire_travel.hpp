#pragma once
#include <atomic>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <array>
#include <optional>

namespace wingflight::vampire {
// The original beast graph emits On/Off on entering/leaving state 53.
// Graph callbacks may occur during the native player update: preserve its
// entire frame, including a start and stop delivered in that same frame.
class BatTravel {
public:
    bool event(std::string_view name) {
        const bool start=name=="BatSprintStart"||name=="BatSprintOn";
        const bool stop=name=="BatSprintOff"||name=="batSprintStop";
        if(!start&&!stop)return false;
        if(start)started_=true;
        active_.store(start);touched_.store(true);return true;
    }
    void reset(){active_=false;touched_=false;started_=false;window_=false;frame_=false;elapsed_=0;timedOut_=false;}
    void beginFrame(float dt,bool ownedNativeFlight,bool animationDriven=false){
        timedOut_=false;
        if(!ownedNativeFlight){reset();return;}
        const bool touched=touched_.exchange(false);
        if(started_.exchange(false)){window_=true;elapsed_=0;}
        if(window_&&std::isfinite(dt))elapsed_+=std::clamp(dt,0.0f,0.1f);
        if(window_&&elapsed_>limit){active_=false;window_=false;timedOut_=true;}
        if(!active_&&!animationDriven)window_=false;
        frame_=active_||touched||(window_&&animationDriven);
    }
    bool afterNative(bool animationDriven=false){
        const bool touched=touched_.exchange(false);
        if(started_.exchange(false)){window_=true;elapsed_=0;}
        frame_=frame_||active_||touched||(window_&&animationDriven);
        if(!active_&&!animationDriven)window_=false;
        return frame_;
    }
    bool yielding() const{return frame_;}
    bool timedOut() const{return timedOut_;}
    static constexpr float limit=4.0f; // Vanilla directional clips last 0.5..0.67s.
private:
    std::atomic_bool active_{},touched_{},started_{};
    bool window_{},frame_{},timedOut_{};
    float elapsed_{};
};
// bhkCharacterStateInAir smooths the already prepared controller.velocityMod,
// even for animation-driven Bat clips. Use that native request directly only
// for owned airborne bat travel; retain native vertical momentum and collisions.
inline std::optional<std::array<float,3>> batVelocity(bool owned,bool yielding,bool driven,bool grounded,
    std::array<float,3> forward,std::array<float,3> requested,std::array<float,3> observed){
    if(!owned||!yielding||!driven||grounded)return {};
    for(auto values:{forward,requested,observed})for(float v:values)if(!std::isfinite(v))return {};
    const float length=std::hypot(forward[0],forward[1]);if(length<.0001f)return {};
    const float x=forward[0]/length,y=forward[1]/length;
    // Despite its name, the native controller's forwardVec points opposite
    // actor forward (verified against actor yaw and native filtered motion).
    return std::array<float,3>{-x*requested[1]-y*requested[0],-y*requested[1]+x*requested[0],observed[2]};
}
}
