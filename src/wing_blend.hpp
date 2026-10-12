#pragma once
#include <algorithm>
#include <cmath>

namespace wingflight {
// Keep the spread sample while incoming glide is mostly obscured. Ramp playback
// as the actual Ni contribution becomes dominant, before the fade completes.
// Positive frequency is required by engine time conversion. No visible seeking.
inline float glidePlaybackScale(float visibleWeight, float loopMultiplier=1) {
    if (!std::isfinite(visibleWeight)) { return 0.01f; }
    const float p=std::clamp((visibleWeight-0.65f)/0.30f,0.0f,1.0f);
    const float base=0.01f+0.99f*p*p*(3-2*p);
    const float q=std::clamp((visibleWeight-0.90f)/0.10f,0.0f,1.0f);
    const float multiplier=std::isfinite(loopMultiplier)?std::clamp(loopMultiplier,0.25f,4.0f):1.0f;
    return base*(1+(multiplier-1)*q*q*(3-2*q));
}
struct WaveWeights {float flap{},fast{},spread{},original{};};
inline WaveWeights waveContributions(float flap,float fast,float ownership){
    return {flap*ownership,(1-flap)*fast*ownership,(1-flap)*(1-fast)*ownership,1-ownership};
}
struct WingWeights { float flap{},glide{},original{1}; };
inline WingWeights contributions(float modeFlap, float ownership) {
    return {modeFlap*ownership,(1-modeFlap)*ownership,1-ownership};
}
// Entry only: keep the original art visible until both incoming clips activate.
// Normal flap/glide blending is unchanged once this reaches one.
class WingEntry {
public:
    void reset() { progress_=0; }
    bool complete() const { return progress_>=1; }
    float value() const { return progress_*progress_*(3-2*progress_); }
    void update(float dt, bool playing, float duration) {
        if (!playing || !std::isfinite(dt) || dt<=0 || !std::isfinite(duration) || duration<=0) { return; }
        progress_=std::min(1.0f,progress_+std::min(dt,0.1f)/duration);
    }
private:
    float progress_{};
};
// Critically damped percentage, integrated analytically. Retain both value and
// velocity when a press reverses a blend; never reset to a clip endpoint.
class WingBlend {
public:
    void reset(float initial=1) { value_=initial; velocity_=0; }
    float value() const { return value_; }
    float update(float dt, bool flap, float responseSeconds) {
        if (!std::isfinite(dt) || dt<=0 || !std::isfinite(responseSeconds) || responseSeconds<=0) { return value_; }
        dt=std::min(dt,0.1f);
        const float target=flap?1.0f:0.0f;
        const float omega=6.0f/responseSeconds;
        const float delta=value_-target, c=velocity_+omega*delta, decay=std::exp(-omega*dt);
        value_=target+(delta+c*dt)*decay;
        velocity_=(velocity_-omega*c*dt)*decay;
        if (value_<0 || value_>1) { value_=std::clamp(value_,0.0f,1.0f); velocity_=0; }
        if (std::abs(value_-target)<0.0001f && std::abs(velocity_)<0.01f) { value_=target;velocity_=0; }
        return value_;
    }
private:
    float value_{1},velocity_{};
};
}
