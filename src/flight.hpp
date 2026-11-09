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
    bool allowOverencumberedFlight{false};
    bool endFlightOnGround{false};
    bool groundWingIdle{false};
    float groundHoverHeight{0.7f}; // Havok units above a retained floor.
    float groundBeatDistance{0.7f}; // Havok units to the floor below for normal-speed ground beats.
    float descentSpeed{3.f}; // Havok units/s for controlled wingbeat and Continuous descent.
    float groundWingIdleSpeedPercent{100.f}; // Visual-only ground beats; manual beats keep full speed.
    float speed{7.0f}, acceleration{4.0f}, drag{1.2f};
    float sprintSpeedMultiplier{1.50f};
    float turnResponse{12.0f};
    float lift{5.5f}, maxRise{10.0f}, gravity{5.0f}, glideFall{3.0f};
    float glideDeceleration{10.0f}; // Maximum reduction in falling speed per second.
    float flapCooldown{0.5f};
    float flapCost{20.0f}, sustainCost{0.0f};
    float flapSpeedMultiplier{1.15f}, maxFlapSpeedMultiplier{1.60f};
    float flapSpeedHoldSeconds{1.50f}, flapSpeedFadeSeconds{3.00f};
    float autoFlapImpulse{6.5f},autoFlapCost{10.0f};
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
inline bool flightLoadAllowed(const Tuning& tuning,bool overencumbered) {
    return tuning.allowOverencumberedFlight || !overencumbered;
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
    float groundDistance{INFINITY}; // Havok units down to the floor; infinity when no hit.
    bool overencumbered{}; // Player's native IsOverEncumbered result.
};
struct Outcome { bool flapped{}, landed{}, aborted{}, released{}; float cost{}; bool highSpeedEntryFlap{},automaticFlapped{}; };

// Floor distance below the feet from a downward ray of `range` Havok units
// that starts `startAbove` above the feet.
inline float rayFloorDistance(float range, float startAbove, float hitFraction) {
    return std::max(0.f, range*hitFraction-startAbove);
}
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
    bool automaticDescent() const { return flying()&&!continuous_&&!highSpeed_&&controlledDescent_; }
    bool automaticHover() const { return flying()&&!groundIdle_&&groundIdleGrace_<=0&&!continuous_&&(mode_==Mode::free||normalGroundHover_||controlledDescent_)&&!highSpeed_; }
    bool normalGroundHover() const { return normalGroundHover_; }
    float descentBeatRate(const Tuning& t,float upSeconds,float downSeconds) const {
        if(!automaticDescent()&&(!normalGroundHover_||!groundDescending_))return 1.f;
        // Keep full wingbeats and steer their speed around the rate whose lift
        // just holds altitude: sinking faster than DescentSpeed beats
        // faster, sinking slower beats slower. Never above normal speed. The
        // same scale drives visible progress, so lift follows the animation.
        // The trim learns what the estimate misses (frame rate, clip timing).
        const float hold=t.gravity*(upSeconds+downSeconds)/t.autoFlapImpulse;
        const float error=descentSink_-std::clamp(t.descentSpeed,.5f,8.f);
        return std::clamp(hold*(1+error/(t.gravity*groundDescentResponse)+descentTrim_),groundDescentMinimumRate,1.f);
    }
    bool groundWingIdle(const Tuning& t) const { return !t.endFlightOnGround&&t.groundWingIdle&&flying()&&!continuous_&&!highSpeed_&&(groundRetained_||groundIdleGrace_>0); }
    bool continuousFlight() const { return flying()&&continuous_; }
    void setContinuousFlight(bool enabled) {
        if(continuous_==enabled)return;
        continuous_=enabled;normalGroundHover_=false;hoverAnchor_=false;hoverIntegral_=0;autoHover_.reset();entryFlapPending_=false;
        speedBoost_=boostHold_=boostFadeAge_=boostFadeStart_=0;
        if(enabled){mode_=Mode::free;velocity_.z=0;}
    }
    Mode mode() const { return mode_; }
    Vec3 velocity() const { return velocity_; }
    // Held glide keeps the engine's movement and gravity. Only arrest descent
    // beyond its gradually tightening fall-speed envelope after native update.
    Vec3 limitGlideVelocity(Vec3 native) const {
        if(glideOnly())native.z=std::max(native.z,-glideFallLimit_);
        return native;
    }
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
    void groundContact(float height,const Tuning& t) {
        if(!flying())return;
        groundDescending_=false;
        groundRetained_=true;groundIdle_=!t.endFlightOnGround&&t.groundWingIdle;
        groundIdleGrace_=groundIdle_?.18f:0;velocity_.z=std::max(0.0f,velocity_.z);
        // Retain the original low-altitude reference and continuous beat
        // scheduling in both K modes. Falling slows beats, never pauses them.
        if(!groundIdle_){
            if(!t.endFlightOnGround&&!continuous_&&!highSpeed_&&velocity_.z<=0&&std::isfinite(height)){
                normalGroundHover_=true;autoHover_.settle(height+t.groundHoverHeight);
            }
            return;
        }
        normalGroundHover_=false;
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
    void beginGlide(Vec3 velocity,const Tuning& t){
        reset();active_=glideOnly_=true;velocity_=velocity;
        glideFallLimit_=std::max(t.glideFall,-velocity.z);
    }
    Outcome tick(float dt, Controls input, Observation observed, const Tuning& t) {
        Outcome result;
        if (!active_) { return result; }
        if (!observed.eligible || !observed.validActor || !flightLoadAllowed(t,observed.overencumbered)) {
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
        groundIdleGrace_=(t.endFlightOnGround||!t.groundWingIdle)?0:std::max(0.f,groundIdleGrace_-dt);
        groundRetained_=retainGroundContact(observed.supported,t);
        if(normalGroundHover_&&(t.endFlightOnGround||t.groundWingIdle||continuous_)){
            normalGroundHover_=false;autoHover_.reset();
        }
        groundIdle_=groundRetained_&&t.groundWingIdle;
        if(groundRetained_)groundContact(observed.height,t);
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
        if(wasFast!=highSpeed_){normalGroundHover_=false;hoverAnchor_=false;hoverIntegral_=0;autoHover_.reset();}
        if (input.cycle && !glideOnly_&&!continuous_) { mode_ = mode_==Mode::flap ? Mode::free : Mode::flap; }
        // A manual beat adds phase-driven lift without changing the main mode.

        if (mode_!=previousMode) {
            hoverAnchor_=false;hoverIntegral_=0;if(!normalGroundHover_)autoHover_.reset();
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
        // Manual mode sinks with automatic beats without requiring Ctrl.
        // Real ground behavior takes precedence. Without enough stamina, retain
        // ordinary glide rather than producing unpaid lift or switching modes.
        controlledDescent_=!glideOnly_&&!continuous_&&!highSpeed_&&!normalGroundHover_&&!groundIdle_
            &&(mode_==Mode::flap||input.down)
            &&observed.stamina-result.cost>=t.autoFlapCost;
        if(controlledDescent_!=wasDescending){
            descentSink_=-velocity_.z;descentTrim_=0;
            autoHover_.reset();autoHover_.observe(observed.height);
        }
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
        // Manual takeoff leaves the floor cycle without changing K's mode.
        if(result.flapped)normalGroundHover_=false;
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
            const float descent=std::clamp(t.descentSpeed,.5f,8.f);
            const float climb=input.up?t.continuousRiseSpeed:input.down?-descent:0;
            if(input.up||input.down){hoverHeight_=observed.height;hoverIntegral_=0;}
            const float error=hoverHeight_-observed.height;
            hoverIntegral_=std::clamp(hoverIntegral_+6.0f*error*dt,-highSpeedHeightCorrectionSpeed,highSpeedHeightCorrectionSpeed);
            const float target=climb+4.0f*error+hoverIntegral_;
            velocity_.z=approach(velocity_.z,target,t.acceleration,dt);
            velocity_.z=std::clamp(velocity_.z,-descent-highSpeedHeightCorrectionSpeed,t.continuousRiseSpeed+highSpeedHeightCorrectionSpeed);
        } else if(glideOnly_) {
            const float change=t.glideDeceleration*dt;
            glideFallLimit_+=std::clamp(t.glideFall-glideFallLimit_,-change,change);
            velocity_.z=std::max(velocity_.z-t.gravity*dt,-glideFallLimit_);
        } else if (mode_ == Mode::flap && !highSpeed_&&!normalGroundHover_&&!controlledDescent_) {
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
            if(!groundIdle_&&groundIdleGrace_<=0&&!observed.wingBusy&&!result.flapped
                &&autoHover_.due(velocity_.z,t.gravity,observed.autoLeadSeconds,
                    observed.autoDownstrokeSeconds,t.autoFlapImpulse)) {
                if(observed.stamina-result.cost-observed.reservedAutoStamina>=t.autoFlapCost) {
                    result.automaticFlapped=true; // Reserve now; pay only on visible downstroke progress.
                } else {
                    mode_=Mode::flap;normalGroundHover_=false;controlledDescent_=false;autoHover_.reset();
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
        // Near the floor below, keep the normal cadence and aim at that floor,
        // so a slope or new floor never inherits an old altitude. Farther away
        // slow down; the band between keeps one beat's apex from flickering.
        if(!normalGroundHover_)groundDescending_=false;
        else if(std::isfinite(observed.groundDistance)&&std::isfinite(observed.height)){
            const float distance=std::max(0.f,observed.groundDistance);
            if(distance<=t.groundBeatDistance)groundDescending_=false;
            else if(distance>t.groundBeatDistance+groundBeatBand)groundDescending_=true;
            if(!groundDescending_)autoHover_.settle(observed.height-distance+t.groundHoverHeight);
        } else if(!observed.supported&&velocity_.z<0)groundDescending_=true;
        // Average out the per-beat swing; start from the real speed on entry.
        if(!groundDescending_&&!automaticDescent()){descentSink_=-velocity_.z;descentTrim_=0;}
        else {
            descentSink_+=(-velocity_.z-descentSink_)*std::min(1.f,dt/groundSinkSmoothing);
            // Learn only near the target, not while the fall is still building.
            const float error=descentSink_-std::clamp(t.descentSpeed,.5f,8.f);
            if(std::abs(error)<1)descentTrim_=std::clamp(descentTrim_+error*dt/(t.gravity*groundDescentTrimSeconds),-.5f,.5f);
        }
        return result;
    }
    // Called once per rendered downstroke sample, after the animation policy.
    // No timer fallback: an absent/held/hidden clip cannot produce phantom lift.
    void applyWingLift(float fraction, bool firstSample, const Tuning& t,bool automatic=false) {
        if(automatic&&!t.endFlightOnGround&&t.groundWingIdle&&(groundRetained_||groundIdleGrace_>0))return;
        if (!active_ || glideOnly_ || highSpeed_ || continuous_ || !std::isfinite(fraction) || fraction<=0) { return; }
        if(!automatic)normalGroundHover_=false;
        if(mode_==Mode::free||(automatic&&(normalGroundHover_||controlledDescent_))){
            if(firstSample){
                if(automatic)autoHover_.automaticDownstroke();
                else {autoHover_.manualClimb();velocity_.z=std::max(0.0f,velocity_.z);}
            }
            const float lift=(automatic?t.autoFlapImpulse:t.lift)*std::min(fraction,1.0f);
            // Cap only the newly added lift. An inherited faster upward velocity
            // must not be abruptly cut to MaxRise. Only a manual stroke keeps
            // the existing fall-arrest behavior; automatic beats add lift gradually.
            // Near a retained floor, automatic beats coast up to the low hover
            // reference instead of spending their whole airborne budget.
            float ceiling=t.maxRise;
            if(automatic&&!t.endFlightOnGround&&!t.groundWingIdle&&autoHover_.grounded())
                ceiling=std::min(ceiling,autoHover_.riseCeiling(t.gravity));
            velocity_.z+=std::min(lift,std::max(0.0f,ceiling-velocity_.z));return;
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
        normalGroundHover_=false;autoHover_.reset();autoHover_.observe(height);
        entryFlapPending_=false;return true;
    }
    void rejectAutomaticStroke(const Tuning& t){
        if(!automaticHover())return;
        mode_=Mode::flap;normalGroundHover_=false;autoHover_.reset();controlledDescent_=false;velocity_.z=std::max(velocity_.z,-t.glideFall);
    }
private:
    bool active_{}, leftGround_{},glideOnly_{},highSpeed_{},continuous_{},groundRetained_{},groundIdle_{};
    bool normalGroundHover_{},groundDescending_{};
    static constexpr float groundBeatBand=.25f; // Havok units above GroundBeatDistance.
    // Sink-speed feedback: seconds of smoothing, seconds to close the error,
    // seconds for the trim to absorb a steady error, and the slowest beat
    // while still falling slower than the target.
    static constexpr float groundSinkSmoothing=.8f,groundDescentResponse=1.5f,groundDescentTrimSeconds=3.f,groundDescentMinimumRate=.25f;
    float descentSink_{},descentTrim_{};
    bool entryFlapPending_{};
    bool controlledDescent_{};
    bool reversing_{};
    float reversalHeading_{};
    Mode mode_{Mode::flap};
    Vec3 velocity_{};
    float glideFallLimit_{};
    float cooldown_{}, age_{}, contact_{},groundIdleGrace_{};
    float speedBoost_{},boostHold_{},boostFadeAge_{},boostFadeStart_{};
    bool hoverAnchor_{};
    float hoverHeight_{},hoverIntegral_{};
    AutoHover autoHover_;
};
}
