#pragma once
#include <algorithm>
#include <cmath>
namespace wingflight {
struct BodyTiming {
    bool enabled{true},banking{true};
    // Internal forward-banking hysteresis, no longer user-facing pose controls.
    static constexpr float enterSpeedRatio=0.35f,exitSpeedRatio=0.20f,enterDelay=0.16f,exitDelay=0.12f;
    float glideSpeed{1.0f},forwardGlideSpeed{1.2f};
};
class BodyMotion {
public:
    void reset(){*this={};}
    bool forward()const{return forward_;}
    int pose()const{return pose_;}
    float glideSpeed()const{return rate_;}
    void update(float dt,bool eligible,float forwardInput,float speed,float base,const BodyTiming& t,
        bool fast=false){
        const bool wanted=eligible&&t.enabled&&forwardInput>0.05f&&speed>base*(forward_?t.exitSpeedRatio:t.enterSpeedRatio);
        if(wanted==forward_)age_=0;else if((age_+=dt)>=(wanted?t.enterDelay:t.exitDelay)){forward_=wanted;age_=0;}
        if(!eligible||!t.enabled)forward_=false;
        int type=forward_?1:0;
        if(fast)type=2;
        // OAR owns only straight/forward poses. Runtime banking is shared with
        // armed flight; selecting an authored bank here would apply it twice.
        pose_=type+1;
        const float target=fast?t.forwardGlideSpeed:t.glideSpeed;
        rate_=target+(rate_-target)*std::exp(-6*dt);
    }
private:bool forward_{};float age_{},rate_{1};int pose_{1};
};
}
