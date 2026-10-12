#pragma once
#include "wing_blend.hpp"

namespace wingflight {
// Original dragon-cycle durations, without allocating or modifying a model.
// The ordinary WingMotion, lift/charge policy and sound marker policy consume
// these clocks exactly as they consume rendered dragon-cycle progress.
struct LogicalWings {
    float phase{},fastPhase{0.233333f},spreadPhase{};
    float rate{0.01f},fastRate{1},spreadRate{1};
    static constexpr float duration=3.166667f,fastDuration=1,spreadDuration=0.533333f;
    WaveWeights weights{0,0,0,1};
    void reset(float entry=0.233333f){*this={};fastPhase=entry;}
    void advance(float dt){
        if(!std::isfinite(dt)||dt<=0)return;dt=std::min(dt,0.1f);
        phase=std::fmod(phase+dt*rate/duration,1.0f);
        fastPhase=std::fmod(fastPhase+dt*fastRate/fastDuration,1.0f);
        spreadPhase=std::fmod(spreadPhase+dt*spreadRate/spreadDuration,1.0f);
    }
    void blend(float flap,float ownership,float fast){
        if(!std::isfinite(flap)||!std::isfinite(ownership)||!std::isfinite(fast))return;
        weights=waveContributions(std::clamp(flap,0.0f,1.0f),std::clamp(fast,0.0f,1.0f),std::clamp(ownership,0.0f,1.0f));
    }
    bool parkPower(){if(weights.flap>0.0001f)return false;phase=0;rate=.01f;return true;}
    bool parkFast(){if(weights.fast>0.0001f)return false;fastPhase=0;return true;}
    void powerRate(float value){if(std::isfinite(value)&&value>=0)rate=value;}
    void loopRate(float fast,float spread){fastRate=glidePlaybackScale(weights.fast,fast);spreadRate=glidePlaybackScale(weights.spread,spread);}
};
}
