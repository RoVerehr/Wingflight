#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdint>
#include "diagnostics.hpp"

namespace wingflight {
struct WingTiming {
    float expandSeconds{0.32f}, upstrokeSeconds{0.40f}, downstrokeSeconds{0.34f};
    float fastUpstrokeSeconds{0.20f}, fastDownstrokeSeconds{0.34f};
    float downstrokeEnd{0.32f}, recoveryRate{0.9f};
    float normalFlapSeconds{1.0f};
    float toFlapSeconds{0.32f}, toGlideSeconds{0.40f};
    float glideEntryPhase{0.233333f};
};
struct WingMotionFrame {
    bool flapActive{}, glideActive{};
    float phase{}, duration{3.166667f}, verticalSpeed{};
    float visibleWeight{1},actualRate{1};
    float automaticRecoverySeconds{-1};
};

struct HoverTiming {
    float upstrokeSeconds{0.65f},downstrokeSeconds{0.34f};
    float recoveryRate{0.9f};
    WingTiming apply(WingTiming timing) const {
        timing.upstrokeSeconds=upstrokeSeconds;
        timing.downstrokeSeconds=downstrokeSeconds;
        timing.recoveryRate=recoveryRate;
        return timing;
    }
};

// Visible motion always progresses forward. Wings may park an entirely invisible
// flap sequence at the raised pose before this policy starts a new entry.
class WingMotion {
public:
    enum class Stroke { waiting, opening, down, recovery };
    enum class Credit { visual,manual,automatic };
    struct Charge {bool due{};Credit kind{Credit::visual};float cost{};std::uint64_t token{};};
    void reset() { *this = {}; }
    void setHover(bool enabled) {
        if(enabled==hovering_)return;
        hovering_=enabled;if(enabled&&!hasManual())urgency_=0;
        if(!enabled && !credits_.empty() && credits_.front().kind==Credit::automatic){
            credits_.pop_front();pending_=static_cast<unsigned>(credits_.size());stroke_=Stroke::recovery;
        }
        if(enabled)wantFlap_=true;
    }
    void setDescentGlide(bool enabled){
        if(enabled==descentGliding_)return;
        descentGliding_=enabled;
        if(!enabled)return;
        // A visible automatic downswing can finish as visual motion during the
        // fade, without further lift. Preserve queued/manual strokes in place.
        const bool finishDown=!credits_.empty()&&stroke_==Stroke::down&&credits_.front().kind!=Credit::manual;
        const bool interrupted=!credits_.empty()&&credits_.front().kind!=Credit::manual;
        std::deque<Beat> retained;
        if(finishDown)retained.push_back({Credit::visual,0,true,credits_.front().token});
        for(auto credit:credits_)if(credit.kind==Credit::manual)retained.push_back(credit);
        credits_=std::move(retained);pending_=static_cast<unsigned>(credits_.size());
        if(interrupted&&!finishDown)stroke_=Stroke::recovery;
        wantFlap_=hasManual();
    }
    void pulse(bool physical=true,bool fastManual=false,float cost=0) {
        urgency_=seenPulse_?std::clamp((0.85f-sincePulse_)/0.55f,0.0f,1.0f):0;
        fastManual_=fastManual&&physical;
        if((hovering_||fastManual_) && physical)urgency_=1;
        seenPulse_=true;sincePulse_=0;
        // An automatic downswing already in progress finishes without being
        // rewound. Begin the requested fast raising immediately afterwards.
        // During raising, replace the automatic beat in place at the same pose.
        const Beat credit{physical?Credit::manual:Credit::visual,physical?cost:0,!physical,++nextToken_};
        if(!credits_.empty() && credits_.front().kind!=Credit::manual && !((hovering_||descentGliding_) && physical && stroke_==Stroke::down)){credits_.front()=credit;}
        else {credits_.push_back(credit);++pending_;}
        if (!wantFlap_) { entryTime_=0;if(!descentGliding_||credits_.size()==1){stroke_=Stroke::waiting;lastPhase_=-1;} }
        normalTime_=descentTime_=0;wantFlap_=true;
    }
    bool wantsFlap() const { return wantFlap_; }
    float rate() const { return rate_; }
    float liftFraction() const { return liftFraction_; }
    bool liftStartsStroke() const { return liftStartsStroke_; }
    float automaticLiftFraction() const { return automaticLiftFraction_; }
    bool automaticLiftStartsStroke() const { return automaticLiftStartsStroke_; }
    void automaticPulse(float cost=0) {
        if(!hovering_||descentGliding_||pending_)return;
        credits_.push_back({Credit::automatic,cost,false,++nextToken_});pending_=1;urgency_=0;
        if(!wantFlap_){entryTime_=0;stroke_=Stroke::waiting;lastPhase_=-1;}
        normalTime_=descentTime_=0;wantFlap_=true;
    }
    float automaticLead(const WingTiming& t,float phase) const {
        if(std::isfinite(phase)&&phase>=0&&phase<t.downstrokeEnd)
            return std::max(0.0f,t.expandSeconds-entryTime_);
        return t.upstrokeSeconds;
    }
    unsigned pending() const { return pending_; }
    float reservedCost(bool manualRequest=false,bool cancelAutomatic=false)const{
        float total=0;
        for(std::size_t i=0;i<credits_.size();++i){const auto& beat=credits_[i];
            if(beat.paid||beat.kind==Credit::visual||(cancelAutomatic&&beat.kind==Credit::automatic))continue;
            if(manualRequest&&i==0&&beat.kind!=Credit::manual
                &&!((hovering_||descentGliding_)&&stroke_==Stroke::down))continue;
            total+=beat.cost;
        }
        return total;
    }
    Charge charge()const{return charge_;}
    void settleCharge(bool paid){
        if(!charge_.due)return;
        for(auto& beat:credits_)if(beat.token==charge_.token){beat.paid=true;
            if(!paid){beat.kind=Credit::visual;beat.cost=0;}break;}
        if(!paid){
            if(charge_.kind==Credit::automatic){automaticLiftFraction_=0;automaticLiftStartsStroke_=false;}
            else {liftFraction_=0;liftStartsStroke_=false;}
        }
        charge_={};
    }
    Stroke stroke() const { return stroke_; }
    float upstrokeSeconds(const WingTiming& t) const {
        return t.upstrokeSeconds+(t.fastUpstrokeSeconds-t.upstrokeSeconds)*urgency_;
    }
    float downstrokeSeconds(const WingTiming& t) const {
        return t.downstrokeSeconds+(t.fastDownstrokeSeconds-t.downstrokeSeconds)*urgency_;
    }
    float raiseRatio(const WingTiming& t) const { return upstrokeSeconds(t)/t.upstrokeSeconds; }
    void update(float dt, const WingMotionFrame& frame, bool continuous, const WingTiming& t) {
        liftFraction_=automaticLiftFraction_=0;liftStartsStroke_=automaticLiftStartsStroke_=false;charge_={};
        if (!std::isfinite(dt) || dt<=0) {
            if(!std::isfinite(dt)||dt<0)diagnostic::report("wing-motion.delta-invalid","skip stroke update",this,{},dt);
            return; }
        dt=std::min(dt,0.1f);
        sincePulse_=std::min(sincePulse_+dt,10.0f);
        setHover(continuous);
        if (!frame.flapActive) {
            wasActive_=false; lastPhase_=-1; entryTime_=0;
            if (pending_) { wantFlap_=true; stroke_=Stroke::waiting; rate_=0.01f; }
            return;
        }
        if (!wasActive_) { entryTime_=0; lastPhase_=-1; wasActive_=true; }
        entryTime_+=dt;
        const bool valid=std::isfinite(frame.phase) && frame.phase>=0 && frame.phase<=1;
        if (!valid) {diagnostic::report("wing-motion.phase-invalid","skip active stroke update",this,{},frame.phase);return;}
        const float previousPhase=lastPhase_;
        const bool wrapped=lastPhase_>=0 && frame.phase+0.01f<lastPhase_;
        lastPhase_=frame.phase;
        // Blend from the current pose to the raised-wing start pose BEFORE
        // accelerating the downstroke. A held sequence still blends its weight.
        const auto priorStroke=stroke_;
        const bool wasDown=stroke_==Stroke::down;
        if (wasDown && pending_ && !credits_.empty() && credits_.front().kind!=Credit::visual && previousPhase>=0 && previousPhase<t.downstrokeEnd
            && frame.phase>previousPhase && !wrapped && rate_>0.011f && frame.actualRate>0.011f
            && std::isfinite(frame.visibleWeight) && frame.visibleWeight>=0.5f) {
            const float fraction=(std::min(frame.phase,t.downstrokeEnd)-previousPhase)/t.downstrokeEnd;
            if(fraction>0&&!strokeLiftStarted_&&!credits_.front().paid)
                charge_={true,credits_.front().kind,credits_.front().cost,credits_.front().token};
            if(credits_.front().kind==Credit::automatic){automaticLiftFraction_=fraction;automaticLiftStartsStroke_=fraction>0&&!strokeLiftStarted_;}
            else {liftFraction_=fraction;liftStartsStroke_=fraction>0&&!strokeLiftStarted_;}
            if(fraction>0)strokeLiftStarted_=true;
        }
        if (pending_ && (stroke_==Stroke::waiting || stroke_==Stroke::recovery)) {
            // Never rush the remainder of a downswing to get back to the top.
            stroke_=frame.phase<t.downstrokeEnd ? Stroke::down : Stroke::opening;
        }
        if (stroke_==Stroke::opening && wrapped) { stroke_=Stroke::down; }
        if (wasDown && (frame.phase>=t.downstrokeEnd || wrapped)) {
            if (pending_) { --pending_;credits_.pop_front(); }
            if(hovering_ && !pending_)urgency_=0;
            stroke_=Stroke::recovery; normalTime_=0;
            // Multiple presses buy multiple power strokes without ever rewinding.
            if (pending_) { stroke_=Stroke::opening; }
        }
        if (stroke_==Stroke::opening && priorStroke!=Stroke::opening) {
            openingSpan_=std::max(0.01f,1.0f-frame.phase);
        }
        // Latch each power stroke's timing. An extra press during its descent
        // queues the next beat without suddenly speeding up this one.
        if (stroke_==Stroke::down && priorStroke!=Stroke::down) {
            activeDownstrokeSeconds_=downstrokeSeconds(t);strokeLiftStarted_=false;
        }
        // Process a loop wrap BEFORE holding at the peak. Otherwise that wrap
        // is forgotten and the following downswing runs at the raising rate.
        if (pending_ && frame.phase<0.04f && entryTime_<t.expandSeconds*raiseRatio(t)) { rate_=0.01f; return; }
        if (pending_) {
            wantFlap_=!descentGliding_||hasManual(); normalTime_=descentTime_=0;
            // Slower as the downstroke progresses: 1.41 -> 0.68 times its
            // average rate, followed by the existing gentle recovery.
            const float resistance=1.045f*(1.35f-0.70f*std::clamp(frame.phase/t.downstrokeEnd,0.0f,1.0f));
            rate_=stroke_==Stroke::opening ? frame.duration*((hovering_||fastManual_)?openingSpan_:1.0f)/upstrokeSeconds(t)
                : frame.duration*t.downstrokeEnd/activeDownstrokeSeconds_*resistance;
            if (stroke_==Stroke::opening) {
                // Do not carry one frame of fast raising across the loop seam
                // into the start of the deliberately slower power downswing.
                rate_=std::min(rate_,std::max(0.01f,(1.0f-frame.phase)*frame.duration/dt));
            }
        } else {
            normalTime_+=dt;
            descentTime_=frame.verticalSpeed < -0.15f ? descentTime_+dt : 0;
            // Spread the remaining return motion over the forecast altitude
            // wait, reserving a normal raising interval for the next request.
            // Keep the original clip moving forward; never park at a fixed pose
            // or wrap into an unrequested downstroke/sound/lift/payment.
            if(continuous) {
                const float wait=std::isfinite(frame.automaticRecoverySeconds)&&frame.automaticRecoverySeconds>=0
                    ?frame.automaticRecoverySeconds:3.0f;
                const float remaining=std::max(0.0f,1.0f-frame.phase);
                const float target=std::min(t.recoveryRate,frame.duration*remaining/(wait+t.upstrokeSeconds));
                rate_=target+(rate_-target)*std::exp(-10.0f*dt);
                // Feedback/contact changes can shorten the prediction suddenly.
                // Preserve half the remaining span even on a clamped hitch frame.
                rate_=std::min(rate_,0.5f*remaining*frame.duration/dt);
            } else { rate_=t.recoveryRate+(rate_-t.recoveryRate)*std::exp(-18.0f*dt); }
            if (descentGliding_ || (!continuous && normalTime_>=t.normalFlapSeconds)) { wantFlap_=false; }
            if(descentGliding_)rate_=0; // fade to flutter; never start an unrequested loop
        }
    }
private:
    struct Beat {Credit kind;float cost;bool paid;std::uint64_t token;};
    bool hasManual()const{return std::any_of(credits_.begin(),credits_.end(),[](const auto& beat){return beat.kind==Credit::manual;});}
    Charge charge_;
    std::uint64_t nextToken_{};
    float liftFraction_{};
    float automaticLiftFraction_{};
    bool liftStartsStroke_{},automaticLiftStartsStroke_{},strokeLiftStarted_{};
    unsigned pending_{};
    std::deque<Beat> credits_;
    bool wantFlap_{}, wasActive_{}, seenPulse_{}, hovering_{},descentGliding_{},fastManual_{};
    Stroke stroke_{Stroke::waiting};
    float lastPhase_{-1}, entryTime_{}, normalTime_{}, descentTime_{}, rate_{0.01f};
    float sincePulse_{10},urgency_{};
    float openingSpan_{1},activeDownstrokeSeconds_{0.34f};
};

inline bool canParkFlap(float graphWeight, float sequenceWeight, bool flapActive, bool glideActive) {
    return flapActive && glideActive && graphWeight==0 && sequenceWeight==0;
}
}
