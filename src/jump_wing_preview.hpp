#pragma once
#include <algorithm>
#include <cmath>
namespace wingflight {
inline float previewEase(float p){p=std::clamp(p,0.f,1.f);return p*p*(3-2*p);}
inline float previewFlutterHandoff(float progress,float jumpAmplitude,float glideAmplitude){return jumpAmplitude+(glideAmplitude-jumpAmplitude)*std::clamp(progress,0.f,1.f);}
inline float previewFlutterAmplitude(bool preview,float jumpAmplitude,float glideAmplitude){return preview?jumpAmplitude:glideAmplitude;}
class JumpWingPreview {
public:
    void reset(){*this={};}
    void arm(){pending_=true;age_=0;}
    float update(float dt,bool airborne,bool eligible,float percent=30,float overOpen=35,float openSeconds=.22f,float settleSeconds=.43f){
        if(!std::isfinite(dt)||dt<=0||!std::isfinite(percent)||!std::isfinite(overOpen)||!std::isfinite(openSeconds)||!std::isfinite(settleSeconds)
            ||openSeconds<=0||settleSeconds<=0)return amount_;
        dt=std::min(dt,.1f);
        const float target=std::clamp(percent,0.f,100.f)/100.f;
        overOpen=std::clamp(overOpen,0.f,100.f);
        const bool changed=target!=target_||overOpen!=overOpen_||openSeconds!=openSeconds_||settleSeconds!=settleSeconds_;
        target_=target;overOpen_=overOpen;openSeconds_=openSeconds;settleSeconds_=settleSeconds;
        if(!eligible){pending_=jumped_=false;}
        else if(pending_){age_+=dt;if(airborne){jumped_=true;pending_=false;motionAge_=0;seed_=amount_;closing_=false;}else if(age_>.6f)pending_=false;}
        if(!airborne)jumped_=false;
        if(eligible&&jumped_){
            if(changed){motionAge_=0;seed_=amount_;closing_=false;}
            const float peak=overOpen_/100;
            motionAge_=std::min(openSeconds_+settleSeconds_,motionAge_+dt);
            if(motionAge_<openSeconds_)amount_=seed_+(peak-seed_)*previewEase(motionAge_/openSeconds_);
            else amount_=peak+(target_-peak)*previewEase((motionAge_-openSeconds_)/settleSeconds_);
        }else{
            if(!closing_){closing_=true;motionAge_=0;seed_=amount_;}
            motionAge_=std::min(openSeconds_,motionAge_+dt);
            amount_=seed_*(1-previewEase(motionAge_/openSeconds_));
        }
        return amount_;
    }
    float amount()const{return amount_;}
private:bool pending_{},jumped_{},closing_{};float age_{},amount_{},motionAge_{},seed_{},target_{.3f};
    float overOpen_{35},openSeconds_{.22f},settleSeconds_{.43f};
};
}
