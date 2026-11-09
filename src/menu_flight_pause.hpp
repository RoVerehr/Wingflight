#pragma once
#include <algorithm>
#include <cmath>
#include "diagnostics.hpp"

namespace wingflight {
// ImGui-based frameworks can freeze Main without creating a paused IMenu.
inline bool worldSimulationPaused(bool menuPaused,bool timeFrozen){return menuPaused||timeFrozen;}
// A closed TweenMenu can precede the camera/control handoff. Suspend the
// existing flight; never restart it, adopt a replacement controller or load it.
class MenuFlightPause {
public:
    void reset(){pending_=expired_=false;elapsed_=0;}
    void tweenEvent(){pending_=true;expired_=false;elapsed_=0;}
    bool update(float dt,bool active,bool menuOpen,bool transitionalCamera,bool gameplayReady,bool unpausedMenu=false) {
        if(!active){reset();return false;}
        if(menuOpen){tweenEvent();return true;}
        // Map/Tween menus may own both camera and input while their world
        // keeps running. The closing event re-arms the usual handoff guard.
        if(unpausedMenu){reset();return false;}
        if(gameplayReady&&!transitionalCamera){reset();return false;}
        if(expired_)return false;
        if(transitionalCamera)pending_=true;
        if(!pending_)return false;
        if(std::isfinite(dt))elapsed_+=std::clamp(dt,0.0f,0.1f);
        if(elapsed_<=handoffLimit)return true;
        diagnostic::report("menu.handoff-timeout","resume normal actor eligibility checks",this,{},elapsed_,handoffLimit);
        pending_=false;expired_=true;return false;
    }
    static constexpr float handoffLimit=1.5f;
private:
    bool pending_{},expired_{};
    float elapsed_{};
};
}
