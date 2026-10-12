#pragma once
#include <algorithm>
#include <cmath>
namespace wingflight {
struct AirAction { bool takeoff{},glide{}; };
class AirInput {
public:
    void reset(){*this={};}
    void cancel(bool held){reset();blocked_=held;}
    AirAction update(float dt,bool held,bool pressed,bool released,bool airPress,bool airborne,bool flying,float holdSeconds){
        AirAction out;
        if(flying){cancel(held);return out;}
        if(blocked_){if(!held)blocked_=false;return out;}
        if(pressed){age_=0;startedAir_=airPress;tracking_=true;long_=false;}
        if(tracking_ && held)age_+=std::clamp(dt,0.0f,0.1f);
        if(tracking_ && held && airborne && age_>=holdSeconds)long_=true;
        if(released){out.takeoff=tracking_&&startedAir_&&!long_&&age_<holdSeconds&&airborne;tracking_=false;}
        out.glide=tracking_&&held&&long_&&airborne;
        if(!airborne && !held)reset();
        return out;
    }
private:bool blocked_{},tracking_{},startedAir_{},long_{};float age_{};
};
class JumpBoost {
public:
    void arm(){age_=0;pending_=true;}
    void reset(){pending_=false;age_=0;}
    float sample(float dt,bool eligible,bool airborne,float upward,float heightMultiplier){
        if(!pending_)return upward;
        age_+=dt;
        if(!eligible||age_>0.5f){reset();return upward;}
        if(airborne&&upward>0.1f){reset();return upward*std::sqrt(heightMultiplier);}
        return upward;
    }
private:bool pending_{};float age_{};
};
}
