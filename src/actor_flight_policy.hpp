#pragma once
#include "flight.hpp"
#include <bit>

namespace wingflight::aerial {
// Every actor has the same physical state. Its AI supplies an intent, not a
// different controller class for followers, companions or ordinary NPCs.
enum class Intent { none, follow, land, combat };
enum class CombatMode { airborne, grounded };
enum class Phase { grounded, takeoff, airborne, landing };
inline bool blocksGroundAnimation(bool owned,std::string_view event){
    return owned&&(nativeJumpEvent(event)||event=="JumpLandEnd"||event=="JumpLandEndDirectional");
}
inline bool blocksNativeLocomotion(bool owned,bool flightCommand,std::string_view event){
    return owned&&!flightCommand&&(event=="moveStart"||event=="moveStop"||event=="SprintStart"||event=="SprintStop");
}
struct LocomotionChange {bool moveStart{},moveStop{},sprintStart{},sprintStop{};};
class Locomotion {
public:
    LocomotionChange update(int mode,bool armed){
        const bool move=!armed&&mode!=1,sprint=move&&mode==3;
        LocomotionChange out{move&&!moving_,!move&&moving_,sprint&&!sprinting_,!sprint&&sprinting_};
        moving_=move;sprinting_=sprint;return out;
    }
private:bool moving_{},sprinting_{};
};
class Pursuit {
public:
    bool update(bool leaderSprint,bool arrived,float gap,float separation){
        // Gap is measured to the live formation goal, not to the player center.
        // A wider exit band avoids alternating flight modes near the boundary.
        const float enter=std::max(8.f,separation*2),exit=std::max(3.f,separation);
        if(catchingUp_){if(gap<=exit)catchingUp_=false;}
        else if(gap>enter)catchingUp_=true;
        return catchingUp_||(leaderSprint&&!arrived);
    }
    bool catchingUp()const{return catchingUp_;}
    void advance(float dt,const Tuning& t){
        float remaining=dt;
        if(hold_>0){const float used=std::min(hold_,remaining);hold_-=used;remaining-=used;}
        if(boost_>0&&remaining>0){age_+=remaining;const float p=std::clamp(age_/t.flapSpeedFadeSeconds,0.f,1.f);boost_=fadeStart_*(1-p*p*(3-2*p));}
    }
    void stroke(const Tuning& t){
        boost_=std::min(boost_+t.flapSpeedMultiplier-1,t.maxFlapSpeedMultiplier-1);
        hold_=t.flapSpeedHoldSeconds;age_=0;fadeStart_=boost_;
    }
    float multiplier()const{return 1+boost_;}
private:bool catchingUp_{};float boost_{},hold_{},age_{},fadeStart_{};
};
struct Settings {
    bool enabled{true};
    unsigned maxActors{32};
    float followDistance{200}; // Skyrim units; conversion belongs to the adapter.
    bool airCombat{true};
};
inline Vec3 add(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 subtract(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 multiply(Vec3 a,float s){return {a.x*s,a.y*s,a.z*s};}
inline float dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline float length(Vec3 a){return std::sqrt(dot(a,a));}
inline bool valid(Vec3 a){return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z);}
inline Vec3 limit(Vec3 a,float maximum){const float l=length(a);return l>maximum&&l>0?multiply(a,maximum/l):a;}

struct FollowEvidence {
    bool safe{},waiting{},scene{},hostile{},targetIsPlayer{},teammateTravel{},managedTravel{};
    bool ownedFlight{},continuingRelationship{},temporaryAction{},otherTarget{};
    bool inCombat{},airCombat{};
    bool preparingFlight{}; // Bounded, Wingflight-requested stance transition only.
};
enum class FollowReason {following,temporaryAnimation,unsafe,waiting,scene,hostile,targetChanged,noTravel,airCombat,groundCombat,takeoffPreparation};
inline const char* followReason(FollowReason r){
    switch(r){
    case FollowReason::following:return "active player-follow intent";
    case FollowReason::temporaryAnimation:return "retain airborne follow during temporary native animation/package";
    case FollowReason::unsafe:return "actor unavailable for follow";
    case FollowReason::waiting:return "explicit wait/rest order";
    case FollowReason::scene:return "scene without an established airborne follow relationship";
    case FollowReason::hostile:return "actor hostile to player";
    case FollowReason::targetChanged:return "follow target changed to another reference";
    case FollowReason::airCombat:return "retain established follower flight for native combat";
    case FollowReason::groundCombat:return "native ground combat requested";
    case FollowReason::takeoffPreparation:return "retain follow during requested native takeoff stance transition";
    default:return "no current player-follow evidence";
    }
}
struct FollowDecision {bool follow{};FollowReason reason{FollowReason::noTravel};};
inline FollowDecision followDecision(FollowEvidence e){
    if(!e.safe)return {false,FollowReason::unsafe};
    if(e.waiting)return {false,FollowReason::waiting};
    if(e.hostile)return {false,FollowReason::hostile};
    if(e.otherTarget)return {false,FollowReason::targetChanged};
    if(e.inCombat){
        if(e.airCombat&&e.ownedFlight&&e.continuingRelationship)return {true,FollowReason::airCombat};
        return {false,FollowReason::groundCombat};
    }
    // A requested stance change can clear followTarget before movement is owned.
    // Keep this separate from flight ownership and airborne combat authority.
    if(e.preparingFlight&&e.continuingRelationship&&!e.scene)return {true,FollowReason::takeoffPreparation};
    // An animation/scene run-once package may temporarily clear followTarget.
    // Retain only an already-owned flight with an independently current follower
    // relationship. This neither grants takeoff nor treats team membership as travel.
    if(e.ownedFlight&&e.continuingRelationship&&e.temporaryAction)return {true,FollowReason::temporaryAnimation};
    if(e.scene)return {false,FollowReason::scene};
    if(e.targetIsPlayer||e.teammateTravel||e.managedTravel)return {true,FollowReason::following};
    return {};
}
inline bool followsPlayer(FollowEvidence e){
    return followDecision(e).follow;
}
inline Vec3 formationOffset(unsigned slot,float yaw,float separation){
    const float side=(static_cast<int>(slot%3)-1)*.9f;
    const float behind=-separation-static_cast<float>(slot/3);
    return {std::cos(yaw)*side+std::sin(yaw)*behind,-std::sin(yaw)*side+std::cos(yaw)*behind,0};
}
inline float pursuitSpeedLimit(bool sprint,const Tuning& tuning,float observedLeaderSpeed){
    const float base=tuning.speed*(sprint?tuning.sprintSpeedMultiplier:1.f)*1.5f;
    return std::clamp(std::max(base,std::max(0.f,observedLeaderSpeed)*(sprint?1.8f:1.6f)),.1f,30.f);
}
inline bool landingContact(bool supported,bool nativeGround,float normalZ,float distanceExcess){
    return supported||(nativeGround&&std::isfinite(normalZ)&&normalZ>.45f
        &&std::isfinite(distanceExcess)&&distanceExcess<=.12f);
}
class LandingBrake {
public:
    static constexpr float safeSpeed=.8f;
    static float probeRange(float verticalSpeed,float gravity){
        const float falling=std::max(0.f,-verticalSpeed);
        return std::clamp(8.f+falling*1.8f+gravity*2.f,8.f,96.f);
    }
    bool due(float velocity,float gravity,float clearance,bool groundKnown,float lead,float downSeconds)const{
        if(!groundKnown||!std::isfinite(clearance)||clearance<0||!std::isfinite(velocity)||velocity>=-safeSpeed)return false;
        const float falling=-velocity,atStroke=falling+gravity*lead;
        const float descent=falling*lead+.5f*gravity*lead*lead+(atStroke+safeSpeed)*.5f*downSeconds;
        return clearance<=descent+.4f;
    }
    void beginStroke(float velocity,float gravity,float remainingFraction,float downSeconds){
        const float remaining=std::clamp(remainingFraction,.01f,1.f);
        impulse_=std::max(0.f,-velocity+gravity*remaining*downSeconds-safeSpeed)/remaining;
    }
    float impulse(float fraction)const{return impulse_*std::clamp(fraction,0.f,1.f);}
private:float impulse_{};
};

struct Frame {
    bool safe{true},capable{true},following{true},leaderFlying{true},supported{};
    bool blocked{};
    Vec3 position{},target{},normal{};
    float groundClearance{100}; // Havok units; never a surface-contact substitute.
    Vec3 targetVelocity{};
};
struct Result {Vec3 velocity{};bool started{},landed{},aborted{};};
enum class Failure {none,unsafeActor,missingWings,position,target,time,velocity};
inline Failure sampleFailure(float dt,const Frame& f){
    if(!f.safe)return Failure::unsafeActor;
    if(!f.capable)return Failure::missingWings;
    if(!valid(f.position))return Failure::position;
    if(!valid(f.target))return Failure::target;
    if(!valid(f.targetVelocity))return Failure::velocity;
    if(!std::isfinite(dt)||dt<0||dt>.15f)return Failure::time;
    return Failure::none;
}
inline const char* failureReason(Failure failure){
    switch(failure){
    case Failure::unsafeActor:return "npc.sample-unsafe-actor";
    case Failure::missingWings:return "npc.sample-missing-wings";
    case Failure::position:return "npc.sample-invalid-position";
    case Failure::target:return "npc.sample-invalid-target";
    case Failure::time:return "npc.sample-invalid-delta";
    case Failure::velocity:return "npc.sample-invalid-target-velocity";
    default:return "npc.sample-valid";
    }
}
// Publish the player's validated game-frame clock as one coherent atomic word.
// NPC Update can be called with zero time, and can run more than once per frame.
// No wall clock: menu pauses and invalid game frames never advance flight.
inline std::uint64_t frameTick(std::uint32_t serial,float dt){
    if(!serial||!std::isfinite(dt)||dt<=0||dt>.15f)return 0;
    return (std::uint64_t{serial}<<32)|std::bit_cast<std::uint32_t>(dt);
}
struct FrameStep {float dt{};bool advance{},fallback{};};
inline FrameStep consumeFrame(std::uint64_t tick,std::uint64_t& last,float callbackDelta){
    if(!tick||tick==last)return {};
    last=tick;
    // Keep nonzero invalid callback values visible to the existing guards.
    if(callbackDelta!=0)return {callbackDelta,true,false};
    return {std::bit_cast<float>(static_cast<std::uint32_t>(tick)),true,true};
}
class Motion {
public:
    Phase phase()const{return phase_;}
    bool active()const{return phase_!=Phase::grounded;}
    bool arrived()const{return arrived_;}
    Vec3 velocity()const{return velocity_;}
    void reset(){*this={};}
    void request(Intent intent){intent_=intent;requestedLanding_=intent==Intent::land;}
    void applyWingLift(float fraction,const Tuning& tuning,bool automatic=true,bool firstSample=false){
        if(!active()||!wingDriven_||!std::isfinite(fraction)||fraction<=0)return;
        if(firstSample&&!automatic)velocity_.z=std::max(0.f,velocity_.z);
        velocity_.z+=std::min((automatic?tuning.autoFlapImpulse:tuning.lift)*std::min(fraction,1.f),std::max(0.f,tuning.maxRise-velocity_.z));
    }
    void applyLandingBrake(float impulse){
        if(phase_==Phase::landing&&std::isfinite(impulse)&&impulse>0&&velocity_.z<-LandingBrake::safeSpeed)
            velocity_.z=std::min(-LandingBrake::safeSpeed,velocity_.z+impulse);
    }
    void limitLandingImpact(){
        if(phase_==Phase::landing)velocity_.z=std::max(velocity_.z,-LandingBrake::safeSpeed);
    }
    Result step(float dt,Frame f,float speed,float acceleration,const Tuning* wingTuning=nullptr,const Tuning& landingTuning=Tuning{}){
        Result out;
        if(sampleFailure(dt,f)!=Failure::none){
            out.aborted=active();reset();return out;
        }
        if(dt==0){out.velocity=velocity_;return out;}
        if(!active()){
            if(!f.following||!f.leaderFlying||!f.supported)return out;
            phase_=Phase::takeoff;intent_=Intent::follow;out.started=true;
        }
        age_+=dt;
        if(!f.leaderFlying||!f.following||requestedLanding_){phase_=Phase::landing;intent_=Intent::land;}
        else if(phase_==Phase::landing){phase_=f.supported?Phase::takeoff:Phase::airborne;intent_=Intent::follow;age_=0;}
        if(phase_==Phase::takeoff&&!f.supported){phase_=Phase::airborne;leftGround_=true;}
        if(phase_==Phase::airborne&&!f.supported)leftGround_=true;
        if(phase_==Phase::landing&&f.supported&&leftGround_){reset();out.landed=true;return out;}
        if(phase_==Phase::takeoff&&age_>3&&f.supported){reset();out.aborted=true;return out;}
        wingDriven_=wingTuning&&phase_!=Phase::landing;
        auto delta=subtract(f.target,f.position);
        const float horizontal=std::hypot(delta.x,delta.y),targetSpeed=std::hypot(f.targetVelocity.x,f.targetVelocity.y);
        if(arrived_){if(horizontal>1.2f||targetSpeed>.5f)arrived_=false;}
        else if(horizontal<.65f&&targetSpeed<.35f)arrived_=true;
        const float distance=wingDriven_?horizontal:length(delta);
        const float brakeSpeed=std::sqrt(2*std::max(.1f,acceleration)*std::max(0.f,distance-.25f));
        auto desired=limit(add(limit(multiply(delta,1.7f),std::min(std::max(speed,.1f),brakeSpeed)),f.targetVelocity),std::max(speed,.1f));
        if(length(delta)<.45f)desired=f.targetVelocity;
        if(arrived_)desired.x=desired.y=0;
        desired.z=std::clamp(desired.z,-std::max(.1f,speed),std::max(.1f,speed));
        if(phase_==Phase::takeoff&&!wingDriven_)desired.z=std::max(desired.z,2.5f);
        if(phase_==Phase::landing){
            // End aerial pursuit immediately. Own only a real fall and its
            // visible terminal braking until the native controller finds support.
            desired.x=desired.y=0;
            velocity_.z=std::max(-30.f,velocity_.z-landingTuning.gravity*dt);
            desired.z=velocity_.z;
            if(f.supported&&!leftGround_){reset();out.landed=true;return out;}
        }
        if(f.blocked&&valid(f.normal)){
            const float square=dot(f.normal,f.normal);
            if(square>.01f){const float into=dot(desired,f.normal);if(into<0)desired=subtract(desired,multiply(f.normal,into/square));}
        }
        if(wingDriven_){
            // Same simulated gravity as player flight; only visible downstroke
            // progress adds lift later. Horizontal steering cannot cancel it.
            velocity_.z-=wingTuning->gravity*dt;
            if(phase_==Phase::takeoff&&f.supported)velocity_.z=std::max(0.f,velocity_.z);
            desired.z=velocity_.z;
        }
        const auto change=limit(subtract(desired,velocity_),std::max(.1f,acceleration)*dt);
        velocity_=add(velocity_,change);
        out.velocity=velocity_;return out;
    }
private:
    Phase phase_{Phase::grounded};Intent intent_{Intent::none};
    Vec3 velocity_{};float age_{};bool leftGround_{},requestedLanding_{},wingDriven_{},arrived_{};
};
}
