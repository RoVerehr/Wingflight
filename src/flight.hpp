#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include "auto_hover.hpp"
#include "diagnostics.hpp"

namespace wingflight {
struct Vec3 { float x{}, y{}, z{}; };
// Keep the existing hover save value; migrate removed modes on load.
enum class Mode { flap=0, free=2 };
inline Mode savedMode(int value) { return value==1 || value==2 ? Mode::free : Mode::flap; }
// Internal high-speed trajectory correction budget, not a descent setting.
inline constexpr float highSpeedHeightCorrectionSpeed=4.0f;
struct Tuning {
    bool flapOnHighSpeedEntry{true};
    bool endFlightOnGround{true};
    float speed{7.0f}, acceleration{4.0f}, drag{2.5f};
    float sprintSpeedMultiplier{1.50f};
    float turnResponse{12.0f};
    float lift{4.5f}, maxRise{7.0f}, gravity{4.0f}, glideFall{2.0f};
    float flapCooldown{0.25f};
    float flapCost{0.0f}, sustainCost{0.0f};
    float flapSpeedMultiplier{1.15f}, maxFlapSpeedMultiplier{1.60f};
    float flapSpeedHoldSeconds{0.80f}, flapSpeedFadeSeconds{1.50f};
    float autoFlapImpulse{5.0f},autoFlapCost{};
    float continuousRiseSpeed{4.0f};
};
struct Controls {
    float right{}, forward{}, yaw{},pitch{};
    bool jumpPressed{},jumpReleased{},airPress{},glideHeld{};
    bool up{}, down{}, flap{}, takeoff{}, cycle{}, land{}, sprint{};
};
struct JumpPress { bool flap{}, takeoff{}; };
// Record where the PRESS happened. Processing a ground press after the game's
// jump update must not turn that same press into a second, airborne jump.
inline JumpPress classifyJumpPress(bool downEdge, bool alreadyHeld, bool airborneAtPress) {
    const bool fresh = downEdge && !alreadyHeld;
    return {fresh, fresh && airborneAtPress};
}
inline bool requestsTakeoff(const Controls& input, bool airborneNow) {
    return input.takeoff && airborneNow;
}
inline bool claimJumpForFlight(bool flying, bool freshPress, bool airborne,
    bool hasWings, bool usableActor, bool hasGraph, bool hasStamina) {
    return flying || (freshPress && airborne && hasWings && usableActor && hasGraph && hasStamina);
}
struct Observation {
    bool eligible{true}, validActor{true}, supported{};
    float stamina{100.0f};
    float height{}; // Havok world units, same units as controller velocity.
    float wingPhase{-1},wingWeight{},downstrokeEnd{0.32f};
    float autoLeadSeconds{0.65f},autoDownstrokeSeconds{0.34f};
    bool wingBusy{};
    float reservedManualStamina{},reservedAutoStamina{};
};
struct Outcome { bool flapped{}, landed{}, aborted{}, released{}; float cost{}; bool highSpeedEntryFlap{},automaticFlapped{}; };

inline float fallHeightInGameUnits(float havokHeight, float inverseWorldScale) {
    return havokHeight * inverseWorldScale;
}

inline bool nativeJumpEvent(std::string_view event) {
    return event == "JumpStandingStart" || event == "JumpDirectionalStart"
        || event == "JumpStart" || event == "JumpFall" || event == "JumpFallDirectional"
        || event == "JumpLand" || event == "JumpLandDirectional" || event == "JumpLandFast"
        || event == "JumpLandSoft" || event == "JumpLandFailSafe";
}
inline float approach(float value, float target, float response, float dt) {
    return target + (value - target) * std::exp(-response * dt);
}
inline void stick(float& x, float& y, float deadzone) {
    const auto length = std::hypot(x, y);
    if (!std::isfinite(length) || length <= deadzone) { x = y = 0; return; }
    const auto scale = (std::min(length, 1.0f) - deadzone) / ((1.0f - deadzone) * length);
    x *= scale; y *= scale;
}

// Engine-independent flight policy. No globals, addresses or combat framework.
class Flight {
public:
    bool active() const { return active_; }
    bool flying() const { return active_&&!glideOnly_; }
    bool glideOnly() const { return active_&&glideOnly_; }
    bool highSpeed() const { return highSpeed_; }
    bool descentGlide() const { return flying()&&!continuous_&&mode_==Mode::free&&!highSpeed_&&controlledDescent_; }
    bool automaticHover() const { return flying()&&!groundRetained_&&!continuous_&&mode_==Mode::free&&!highSpeed_&&!controlledDescent_; }
    bool continuousFlight() const { return flying()&&continuous_; }
    void setContinuousFlight(bool enabled) {
        if(continuous_==enabled)return;
        continuous_=enabled;hoverAnchor_=false;hoverIntegral_=0;autoHover_.reset();entryFlapPending_=false;
        speedBoost_=boostHold_=boostFadeAge_=boostFadeStart_=0;
        if(enabled){mode_=Mode::free;velocity_.z=0;}
    }
    Mode mode() const { return mode_; }
    Vec3 velocity() const { return velocity_; }
    float speedMultiplier() const { return 1+speedBoost_; }
    float horizontalMultiplier(bool sprint, const Tuning& t) const {
        return (sprint?t.sprintSpeedMultiplier:1.0f)*speedMultiplier();
    }
    float hoverTarget() const { return highSpeed_||continuous_?hoverHeight_:autoHover_.base(); }
    float hoverExcursion() const { return highSpeed_||continuous_?0:autoHover_.offset(); }
    float automaticRecoverySeconds(float lead,float downSeconds,const Tuning& t) const {
        return automaticHover()?autoHover_.requestDelay(velocity_.z,t.gravity,lead,downSeconds,t.autoFlapImpulse):-1;
    }
    bool readyToLand() const { return active_ && (glideOnly_ || (leftGround_ && age_ > 0.1f)); }
    bool retainGroundContact(bool supported,const Tuning& t) const { return flying()&&supported&&!t.endFlightOnGround; }
    // Cosmetic gates must retain full-flight poses/audio on a floor without
    // falsifying the supporting surface used by physics, damage or landing.
    bool presentationSupported(bool supported,const Tuning& t) const { return supported&&!retainGroundContact(supported,t); }
    // A real floor absorbs descent; it must not store a dive or an old altitude
    // correction. Keep upward manual lift and horizontal flight movement intact.
    void groundContact(float height) {
        if(!flying())return;
        groundRetained_=true;velocity_.z=std::max(0.0f,velocity_.z);
        hoverAnchor_=false;hoverIntegral_=0;autoHover_.reset();
        if(std::isfinite(height)){hoverHeight_=height;autoHover_.observe(height);}
    }
    void restoreMode(Mode mode) { mode_ = mode; hoverAnchor_=false; }
    void reset() { *this = Flight{}; }
    void begin(Vec3 velocity, bool takeoff, const Tuning& t) {
        reset(); active_ = true; velocity_ = velocity;
        if (takeoff) { cooldown_ = t.flapCooldown; } // Lift waits for the visible downstroke.
        velocity_.z = std::clamp(velocity_.z, -t.glideFall, t.maxRise);
    }
    void beginGlide(Vec3 velocity,const Tuning& t){begin(velocity,false,t);glideOnly_=true;}
    Outcome tick(float dt, Controls input, Observation observed, const Tuning& t) {
        Outcome result;
        if (!active_) { return result; }
        if (!observed.eligible || !observed.validActor) {
            reset(); result.aborted = true; return result;
        }
        // Manual exit immediately returns all motion/fall handling to Skyrim.
        // It wins over an impulse/mode switch in the same input batch.
        if (input.land) { reset(); result.released=true; return result; }
        // External glide does not need a takeoff grace period: it starts only
        // in air. Give physical support priority over release/freefall events.
        if(glideOnly_ && observed.supported){reset();result.landed=true;return result;}
        if(glideOnly_ && !input.glideHeld){reset();result.released=true;return result;}
        if (!std::isfinite(dt) || dt <= 0) {
            if(!std::isfinite(dt)||dt<0)diagnostic::report("flight.delta-invalid","skip simulation frame",this,{},dt);
            return result; }
        dt = std::min(dt, 0.1f);  // Never catch up a loading hitch with a large impulse.
        age_ += dt; cooldown_ = std::max(0.0f, cooldown_ - dt);
        groundRetained_=retainGroundContact(observed.supported,t);
        if(groundRetained_)groundContact(observed.height);
        if (!observed.supported) { leftGround_ = true; }
        if (!groundRetained_ && observed.supported && age_ > 0.3f && (velocity_.z <= 0 || age_ > 1.0f)) {
            contact_ += dt;
            if (contact_ >= 0.06f && (leftGround_ || age_ > 1.0f)) {
                reset(); result.landed = true; return result;
            }
        } else { contact_ = 0; }
        const auto previousMode=mode_;
        const bool wasFast=highSpeed_;
        const bool wasDescending=controlledDescent_;
        highSpeed_=!glideOnly_&&input.sprint&&std::hypot(input.right,input.forward)>0.05f;
        if(!highSpeed_ || continuous_ || !t.flapOnHighSpeedEntry)entryFlapPending_=false;
        else if(!wasFast)entryFlapPending_=true;
        if(wasFast!=highSpeed_){hoverAnchor_=false;hoverIntegral_=0;autoHover_.reset();}
        if (input.cycle && !glideOnly_&&!continuous_) { mode_ = mode_==Mode::flap ? Mode::free : Mode::flap; }
        // A manual beat adds phase-driven lift without changing the main mode.

        if (mode_!=previousMode) {
            hoverAnchor_=false;hoverIntegral_=0;autoHover_.reset();
            if(mode_==Mode::free) { speedBoost_=boostHold_=boostFadeAge_=boostFadeStart_=0; }
        }
        if (mode_ == Mode::free) {
            result.cost = t.sustainCost * dt;
            if (observed.stamina < result.cost) {
                if(continuousFlight()){
                    diagnostic::report("continuous.stamina-exhausted","release continuous flight",this,"SustainPerSecond",observed.stamina,result.cost);
                    reset();result.aborted=true;result.cost=0;return result;
                }
                mode_ = Mode::flap; result.cost = 0;
            }
        }
        controlledDescent_=!glideOnly_&&mode_==Mode::free&&!highSpeed_&&input.down;
        // Decay existing speed smoothly; a new accepted sprint beat refreshes
        // the hold and adds one bounded increment to the current bonus.
        float fadeDt=dt;
        if (boostHold_>0) { const float used=std::min(boostHold_,fadeDt);boostHold_-=used;fadeDt-=used; }
        if (speedBoost_>0 && fadeDt>0) {
            boostFadeAge_+=fadeDt;
            const float p=std::clamp(boostFadeAge_/t.flapSpeedFadeSeconds,0.0f,1.0f);
            speedBoost_=boostFadeStart_*(1-p*p*(3-2*p));
        }
        // Once native sprint + movement has actually entered high speed, request
        // the SAME beat as Space. Wait out an existing cooldown, cancel on exit,
        // and coalesce a simultaneous manual press into this one accepted beat.
        const bool entryRequest=entryFlapPending_ && cooldown_<=0;
        if(entryRequest)entryFlapPending_=false;
        if ((input.flap || entryRequest) && !glideOnly_ && !continuous_ && cooldown_ <= 0
            && observed.stamina-result.cost-observed.reservedManualStamina >= t.flapCost) {
            // Accept the beat now; its actual animation progress supplies lift later.
            cooldown_ = t.flapCooldown; result.flapped = true; // Paid by the visible downstroke.
            result.highSpeedEntryFlap=entryRequest;
            if (highSpeed_) {
                speedBoost_=std::min(speedBoost_+t.flapSpeedMultiplier-1,t.maxFlapSpeedMultiplier-1);
                boostHold_=t.flapSpeedHoldSeconds;boostFadeAge_=0;boostFadeStart_=speedBoost_;
            }
        }
        float x = input.right, y = input.forward;
        const auto magnitude = std::hypot(x, y);
        if (magnitude > 1) { x /= magnitude; y /= magnitude; }
        const float sine = std::sin(input.yaw), cosine = std::cos(input.yaw);
        const float horizontalScale=horizontalMultiplier(input.sprint,t);
        const float forwardPlanar=y*(highSpeed_?std::cos(input.pitch):1.0f);
        const float targetX = (x * cosine + forwardPlanar * sine) * t.speed * horizontalScale;
        const float targetY = (forwardPlanar * cosine - x * sine) * t.speed * horizontalScale;
        const float fastVertical=highSpeed_?(-y*std::sin(input.pitch)*t.speed*horizontalScale):0;
        if (magnitude>0) {
            // Heading responds independently of acceleration. Retain horizontal
            // speed through a turn instead of braking both world axes to zero.
            const float oldSpeed=std::hypot(velocity_.x,velocity_.y);
            const float targetSpeed=std::hypot(targetX,targetY);
            const float heading=std::atan2(targetY,targetX);
            // Near-opposite requests brake through zero rather than inventing
            // a lateral U-turn. Keep this decision until the new velocity is
            // established, or the player changes the requested direction.
            if(reversing_ && (targetSpeed<=0.001f ||
                std::abs(std::remainder(heading-reversalHeading_,6.28318530718f))>0.5235987756f))
                reversing_=false;
            if(!reversing_ && oldSpeed>0.001f && targetSpeed>0.001f &&
                (velocity_.x*targetX+velocity_.y*targetY)/(oldSpeed*targetSpeed)<-0.95f){
                reversing_=true;reversalHeading_=heading;
            }
            if(reversing_){
                velocity_.x=approach(velocity_.x,targetX,t.acceleration,dt);
                velocity_.y=approach(velocity_.y,targetY,t.acceleration,dt);
                if(std::hypot(velocity_.x-targetX,velocity_.y-targetY)<=
                    std::max(0.001f,targetSpeed*0.005f))reversing_=false;
            }else{
            float angle=heading;
            if (oldSpeed>0.001f) {
                const float oldHeading=std::atan2(velocity_.y,velocity_.x);
                const float delta=std::remainder(heading-oldHeading,6.28318530718f);
                angle=oldHeading+delta*(1-std::exp(-t.turnResponse*dt));
            }
            const float speed=approach(oldSpeed,targetSpeed,t.acceleration,dt);
            velocity_.x=std::cos(angle)*speed;velocity_.y=std::sin(angle)*speed;
            }
        } else {
            reversing_=false;
            velocity_.x=approach(velocity_.x,0,t.drag,dt);
            velocity_.y=approach(velocity_.y,0,t.drag,dt);
        }
        if(continuousFlight()&&!highSpeed_) {
            if(!std::isfinite(observed.height)) {
                diagnostic::report("flight.height-nonfinite","abort flight",this,"continuous flight",observed.height);
                reset();result.aborted=true;return result;
            }
            if(!hoverAnchor_){hoverHeight_=observed.height;hoverIntegral_=0;hoverAnchor_=true;}
            const float climb=input.up?t.continuousRiseSpeed:input.down?-t.glideFall:0;
            if(input.up||input.down){hoverHeight_=observed.height;hoverIntegral_=0;}
            const float error=hoverHeight_-observed.height;
            hoverIntegral_=std::clamp(hoverIntegral_+6.0f*error*dt,-highSpeedHeightCorrectionSpeed,highSpeedHeightCorrectionSpeed);
            const float target=climb+4.0f*error+hoverIntegral_;
            velocity_.z=approach(velocity_.z,target,t.acceleration,dt);
            velocity_.z=std::clamp(velocity_.z,-t.glideFall-highSpeedHeightCorrectionSpeed,t.continuousRiseSpeed+highSpeedHeightCorrectionSpeed);
        } else if (glideOnly_ || (mode_ == Mode::flap && !highSpeed_)) {
            velocity_.z = std::max(velocity_.z - t.gravity * dt, -t.glideFall);
        } else if(!highSpeed_) {
            if(!std::isfinite(observed.height)) {
                diagnostic::report("flight.height-nonfinite","abort flight",this,"free flight",observed.height);
                reset();result.aborted=true;return result;
            }
            // Both modes integrate their own momentum using the same gravity.
            // Native gravity is leased off for the owned full-flight controller.
            // Do not feed an engine/external velocity rewrite back into this state.
            autoHover_.observe(observed.height);
            velocity_.z-=t.gravity*dt;
            // Track the current altitude during descent. On release, preserve
            // an ongoing manual climb while scheduling hover at the new height.
            if(controlledDescent_||wasDescending){
                autoHover_.descend(observed.height);
                if(!controlledDescent_&&velocity_.z>0)autoHover_.manualClimb();
            }
            if(result.flapped)autoHover_.manualClimb();
            if(controlledDescent_) {
                // Cap downward momentum only; Space lift must remain effective.
                velocity_.z=std::max(velocity_.z,-t.glideFall);
            } else if(!groundRetained_&&!observed.wingBusy&&!result.flapped
                &&autoHover_.due(velocity_.z,t.gravity,observed.autoLeadSeconds,
                    observed.autoDownstrokeSeconds,t.autoFlapImpulse)) {
                if(observed.stamina-result.cost-observed.reservedAutoStamina>=t.autoFlapCost) {
                    result.automaticFlapped=true; // Reserve now; pay only on visible downstroke progress.
                } else {
                    mode_=Mode::flap;autoHover_.reset();
                    velocity_.z=std::max(velocity_.z,-t.glideFall);
                }
            }
        } else {
            if (!std::isfinite(observed.height)) {
                diagnostic::report("flight.height-nonfinite","abort flight",this,"high speed",observed.height);
                reset();result.aborted=true;return result; }
            if (!hoverAnchor_) {
                hoverHeight_=observed.height;hoverIntegral_=0;hoverAnchor_=true;
            }
            const float climb=fastVertical;
            // High speed is controlled exclusively by camera pitch/yaw.
            hoverHeight_+=climb*dt;
            const float error=hoverHeight_-observed.height;
            // Actual-height feedback compensates gravity applied between our
            // velocity writes. Clamp integral/velocity; never teleport/no-clip.
            hoverIntegral_=std::clamp(hoverIntegral_+6.0f*error*dt,-highSpeedHeightCorrectionSpeed,highSpeedHeightCorrectionSpeed);
            const float limit=t.speed*horizontalScale+highSpeedHeightCorrectionSpeed;
            velocity_.z=std::clamp(climb+4.0f*error+hoverIntegral_,-limit,limit);
        }
        if(groundRetained_)velocity_.z=std::max(0.0f,velocity_.z);
        return result;
    }
    // Called once per rendered downstroke sample, after the animation policy.
    // No timer fallback: an absent/held/hidden clip cannot produce phantom lift.
    void applyWingLift(float fraction, bool firstSample, const Tuning& t,bool automatic=false) {
        if(automatic&&groundRetained_)return;
        if (!active_ || glideOnly_ || highSpeed_ || continuous_ || !std::isfinite(fraction) || fraction<=0) { return; }
        if(mode_==Mode::free){
            // Descend suppresses automatic lift, never a requested Space stroke.
            if(controlledDescent_&&automatic)return;
            if(firstSample){
                if(automatic)autoHover_.automaticDownstroke();
                else {autoHover_.manualClimb();velocity_.z=std::max(0.0f,velocity_.z);}
            }
            const float lift=(automatic?t.autoFlapImpulse:t.lift)*std::min(fraction,1.0f);
            // Cap only the newly added lift. An inherited faster upward velocity
            // must not be abruptly cut to MaxRise. Only a manual stroke keeps
            // the existing fall-arrest behavior; automatic beats add lift gradually.
            velocity_.z+=std::min(lift,std::max(0.0f,t.maxRise-velocity_.z));return;
        }
        if(automatic)return;
        if (firstSample) { velocity_.z=std::max(0.0f,velocity_.z); }
        velocity_.z=std::min(velocity_.z+t.lift*std::min(fraction,1.0f),t.maxRise);
    }
    // A resolved ceiling contact must not leave stored upward momentum.
    void ceilingContact() { if (active_) { velocity_.z = std::min(velocity_.z, 0.0f); } }
    // Resume at the skill's actual destination; never pull toward the old
    // hover anchor or inherit the native dash's large temporary velocity.
    bool resumeAfterNativeTravel(float height) {
        if(!active_||!std::isfinite(height))return false;
        velocity_={};reversing_=false;hoverAnchor_=false;hoverIntegral_=0;contact_=0;
        autoHover_.reset();autoHover_.observe(height);
        entryFlapPending_=false;return true;
    }
    void rejectAutomaticStroke(const Tuning& t){
        if(!automaticHover())return;
        mode_=Mode::flap;autoHover_.reset();controlledDescent_=false;velocity_.z=std::max(velocity_.z,-t.glideFall);
    }
private:
    bool active_{}, leftGround_{},glideOnly_{},highSpeed_{},continuous_{},groundRetained_{};
    bool entryFlapPending_{};
    bool controlledDescent_{};
    bool reversing_{};
    float reversalHeading_{};
    Mode mode_{Mode::flap};
    Vec3 velocity_{};
    float cooldown_{}, age_{}, contact_{};
    float speedBoost_{},boostHold_{},boostFadeAge_{},boostFadeStart_{};
    bool hoverAnchor_{};
    float hoverHeight_{},hoverIntegral_{};
    AutoHover autoHover_;
};
}
