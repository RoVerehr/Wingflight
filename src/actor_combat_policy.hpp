#pragma once
#include "actor_flight_policy.hpp"

namespace wingflight::aerial {
enum class CombatStyle { unknown, melee, ranged };
inline const char* combatStyleName(CombatStyle style){
    return style==CombatStyle::melee?"melee":style==CombatStyle::ranged?"ranged":"awaiting native equipment";
}
struct CombatRange {float minimum{},optimal{},maximum{};};
inline float forwardAlignment(Vec3 direction,float yaw){
    if(!valid(direction)||!std::isfinite(yaw))return -1.f;
    const float horizontal=std::hypot(direction.x,direction.y);
    return horizontal>.01f?(direction.x*std::sin(yaw)+direction.y*std::cos(yaw))/horizontal:0.f;
}
inline bool forwardSprintAllowed(bool alreadyFast,Vec3 desired,Vec3 velocity,float yaw){
    if(!valid(desired)||!valid(velocity)||!std::isfinite(yaw))return false;
    const float threshold=alreadyFast?.4f:.6f;
    if(forwardAlignment(desired,yaw)<threshold)return false;
    // A newly forward goal must not reinstate sprint while the actor is still
    // moving backward. Near rest can start a forward acceleration normally.
    return std::hypot(velocity.x,velocity.y)<.3f||forwardAlignment(velocity,yaw)>=threshold;
}
struct RetreatSpeed {bool retreating{},urgent{};float limit{};};
class RangedRetreat {
public:
    RetreatSpeed update(CombatStyle style,Vec3 desired,Vec3 enemy,float minimumRange,float ordinarySpeed){
        if(style!=CombatStyle::ranged||!valid(desired)||!valid(enemy)){urgent_=false;return {};}
        const float travel=std::hypot(desired.x,desired.y),distanceXY=std::hypot(enemy.x,enemy.y);
        const bool away=travel>.15f&&distanceXY>.01f
            &&(desired.x*enemy.x+desired.y*enemy.y)<-.1f*travel*distanceXY;
        if(!away){urgent_=false;return {};}
        const float closeRange=std::isfinite(minimumRange)&&minimumRange>.1f?minimumRange:1.8f;
        const float distance=length(enemy);
        if(urgent_){if(distance>closeRange+std::max(.75f,closeRange*.25f))urgent_=false;}
        else if(distance<closeRange)urgent_=true;
        const float speed=std::isfinite(ordinarySpeed)?std::max(.1f,ordinarySpeed):.1f;
        return {true,urgent_,speed*(urgent_?1.f:.5f)};
    }
private:bool urgent_{};
};
// All lengths below are Havok world units. Native equipment, not inventory
// ownership or a follower class, selects the style and usable attack range.
inline CombatRange combatRange(CombatStyle style,float minimum,float optimal,float maximum){
    const bool melee=style==CombatStyle::melee;
    if(!std::isfinite(maximum)||maximum<=.1f)maximum=melee?1.8f:12.f;
    maximum=std::clamp(maximum,.2f,45.f);
    if(!std::isfinite(minimum)||minimum<0)minimum=0;
    minimum=std::min(minimum,maximum*.7f);
    if(!std::isfinite(optimal)||optimal<=minimum)optimal=maximum*(melee?.65f:.7f);
    optimal=std::clamp(optimal,std::max(.1f,minimum),maximum*.85f);
    return {minimum,optimal,maximum};
}
inline Vec3 combatGoal(Vec3 actor,Vec3 target,CombatRange range,CombatStyle style,float yaw){
    auto away=subtract(actor,target);away.z=0;
    const float horizontal=std::hypot(away.x,away.y);
    away=horizontal>.01f?multiply(away,1/horizontal):Vec3{-std::sin(yaw),-std::cos(yaw),0};
    // A dead band avoids orbiting or shuffling at a valid firing position.
    const float low=style==CombatStyle::melee?range.optimal*.7f:std::max(range.minimum,range.optimal*.8f);
    const float high=std::min(range.maximum*.9f,range.optimal*1.15f);
    const float distance=horizontal>=low&&horizontal<=high?horizontal:range.optimal;
    auto goal=add(target,multiply(away,distance));
    // Keep grounded melee opponents within vertical reach; ranged flyers can
    // keep their altitude only while total distance stays inside weapon range.
    const float rise=style==CombatStyle::melee?.15f:
        std::min(4.f,std::sqrt(std::max(0.f,range.maximum*range.maximum*.64f-distance*distance)));
    goal.z=target.z+std::clamp(actor.z-target.z,.15f,std::max(.15f,rise));
    return goal;
}
// Retain only a same-target ranged spacing observation. Auxiliary spells never
// grant an attack intent, chase a distant enemy, or move the held altitude.
class CombatSpacing {
public:
    void observe(std::uint32_t target,CombatStyle style,CombatRange range){
        if(target!=target_){target_=target;ranged_=false;}
        if(style!=CombatStyle::unknown){ranged_=style==CombatStyle::ranged;range_=range;}
    }
    bool available(std::uint32_t target)const{return target&&target==target_&&ranged_;}
    float minimum()const{return range_.minimum;}
    Vec3 holdGoal(std::uint32_t target,Vec3 actor,Vec3 enemy,float yaw)const{
        if(!available(target)||!valid(actor)||!valid(enemy))return actor;
        const float distance=std::hypot(actor.x-enemy.x,actor.y-enemy.y);
        if(distance>=std::max(range_.minimum,range_.optimal*.8f))return actor;
        auto goal=combatGoal(actor,enemy,range_,CombatStyle::ranged,yaw);
        goal.z=actor.z;return goal;
    }
private:std::uint32_t target_{};CombatRange range_{};bool ranged_{};
};
enum class CombatAction { follow, hold, engage, land };
inline bool combatFlightWanted(bool enabled,bool following,bool leaderFlying,bool owned,CombatAction action){
    if(!enabled||!following||action==CombatAction::land)return false;
    return action==CombatAction::follow?leaderFlying:owned;
}
class CombatSession {
public:
    CombatAction update(bool enabled,bool inCombat,bool ready,bool fleeing,float){
        if(!inCombat){*this={};return CombatAction::follow;}
        if(enabled!=enabled_){fallback_=false;enabled_=enabled;}
        if(!enabled)return CombatAction::land;
        if(fleeing)fallback_=true;
        // Target/equipment gaps also occur during wards, summons and swaps.
        // Keep the owned flight until native combat or explicit intent ends it.
        return fallback_?CombatAction::land:ready?CombatAction::engage:CombatAction::hold;
    }
private:bool enabled_{},fallback_{};
};
}
