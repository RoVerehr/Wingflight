#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace wingflight::impact {
struct Settings { bool enabled{true},damageActors{true};float multiplier{1},minSpeed{8}; };
struct Vector {float x{},y{},z{};};
inline bool finite(Vector v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
struct Contact {std::uintptr_t id{};Vector normal;};
struct Hit {std::uintptr_t id{};float speed{},damage{};};
inline constexpr std::size_t maxContacts=64;
struct Result {std::array<Hit,maxContacts> hits{};std::size_t count{};float playerDamage{};};
inline float closingSpeed(Vector velocity,Vector normal){
    if(!finite(velocity)||!finite(normal))return 0;
    const float length=std::hypot(normal.x,normal.y,normal.z);
    if(length<.001f)return 0;
    return std::max(0.f,-(velocity.x*normal.x+velocity.y*normal.y+velocity.z*normal.z)/length);
}
inline float damageFor(float speed,const Settings& s){
    if(!s.enabled||!std::isfinite(speed)||!std::isfinite(s.multiplier)||!std::isfinite(s.minSpeed)
        ||s.multiplier<=0||s.minSpeed<0||speed<=s.minSpeed)return 0;
    const double excess=static_cast<double>(speed)-s.minSpeed;
    return static_cast<float>(std::min(100000.0,5.0*excess*excess*s.multiplier));
}
// Bounded, allocation-free contact episodes. Multiple triangles on one object
// count as one impact. Contact must be absent for 0.15s before it can re-arm.
class Tracker {
public:
    void reset(){*this={};}
    void commit(Vector command){velocity_=command;armed_=finite(command);}
    Result sample(float dt,std::span<const Contact> contacts,const Settings& settings){
        Result result;
        if(!settings.enabled||settings.multiplier<=0||!std::isfinite(dt)||dt<=0||dt>.1f){reset();return result;}
        cooldown_=std::max(0.f,cooldown_-dt);
        for(auto& e:episodes_)if(e.id)e.absent+=dt;
        std::array<Hit,maxContacts> current{};std::size_t count=0;
        for(const auto& c:contacts.first(std::min(contacts.size(),maxContacts))){
            if(!c.id||!finite(c.normal))continue;
            std::size_t i=0;for(;i<count&&current[i].id!=c.id;++i){}
            if(i==count){current[count++].id=c.id;}
            current[i].speed=std::max(current[i].speed,closingSpeed(velocity_,c.normal));
        }
        for(std::size_t i=0;i<count;++i){
            auto& hit=current[i];Episode* entry=nullptr;
            for(auto& e:episodes_)if(e.id==hit.id){entry=&e;break;}
            // Account for this frame only after checking the previous absence.
            const bool fresh=!entry||entry->absent-dt>=.15f;
            if(!entry)for(auto& e:episodes_)if(!e.id||e.absent-dt>=.15f){entry=&e;break;}
            if(!entry)continue;
            *entry={hit.id,0};
            if(!fresh||!armed_||cooldown_>0)continue;
            hit.damage=damageFor(hit.speed,settings);
            if(hit.damage>0){result.hits[result.count++]=hit;result.playerDamage=std::max(result.playerDamage,hit.damage);}
        }
        if(result.count)cooldown_=.35f;
        return result;
    }
private:
    struct Episode{std::uintptr_t id{};float absent{};};
    std::array<Episode,maxContacts*2> episodes_{};
    Vector velocity_{};float cooldown_{};bool armed_{};
};
}
