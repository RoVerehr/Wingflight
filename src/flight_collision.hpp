#pragma once
#include "collision_policy.hpp"
#include "diagnostics.hpp"
#include "flight_audio.hpp"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace wingflight {
// Reads the player proxy's solved contact manifold. No new physics hooks,
// scene scans, ray casts or collision filters. Damage is applied outside the
// Havok read lock, on the existing player update thread.
class FlightCollision {
public:
    void reset(){tracker_.reset();controller_=nullptr;}
    void commit(RE::bhkCharacterController* controller,float x,float y,float z){
        if(controller_!=controller){tracker_.reset();controller_=controller;}
        tracker_.commit({x,y,z});
    }
    bool update(RE::PlayerCharacter* player,RE::bhkCharacterController* controller,float dt,const impact::Settings& settings){
        if(!settings.enabled||settings.multiplier<=0||!controller||!std::isfinite(dt)||dt<=0||dt>.1f){reset();return false;}
        if(controller_!=controller){tracker_.reset();controller_=controller;}
        auto proxyController=skyrim_cast<RE::bhkCharProxyController*>(controller);
        auto proxy=proxyController?proxyController->GetCharacterProxy():nullptr;
        auto world=controller->GetHavokWorld();
        if(!proxy||!world){diagnostic::report("collision.contacts-unavailable","skip collision damage; flight remains available",controller);tracker_.reset();return false;}
        impact::Result result;
        std::array<RE::NiPointer<RE::Actor>,impact::maxContacts> actors{};
        {
            RE::BSReadLockGuard lock(world->worldLock);
            if(!proxy->shapePhantom){tracker_.reset();return false;}
            const auto own=proxy->shapePhantom->GetCollidable();
            const auto size=proxy->manifold.size();
            if(size>impact::maxContacts)diagnostic::report("collision.contact-limit","inspect first 64 physical contacts",controller,{},static_cast<float>(size));
            std::array<impact::Contact,impact::maxContacts> contacts{};
            std::array<const RE::hkpCollidable*,impact::maxContacts> bodies{};
            std::size_t count=0;
            const float tolerance=std::isfinite(proxy->keepDistance)?std::clamp(proxy->keepDistance,0.f,.1f)+.02f:.02f;
            for(std::size_t i=0;i<std::min<std::size_t>(size,impact::maxContacts);++i){
                const auto& point=proxy->manifold[static_cast<std::int32_t>(i)];const auto& n=point.contact.separatingNormal.quad;
                if(!std::isfinite(n.m128_f32[3])||n.m128_f32[3]>tolerance)continue;
                const RE::hkpCollidable* other=nullptr;float sign=1;
                if(point.rootCollidableA==own)other=point.rootCollidableB;
                else if(point.rootCollidableB==own){other=point.rootCollidableA;sign=-1;}
                else continue;
                if(!other||other==own)continue;
                bodies[count]=other;
                contacts[count++]={reinterpret_cast<std::uintptr_t>(other),{sign*n.m128_f32[0],sign*n.m128_f32[1],sign*n.m128_f32[2]}};
            }
            result=tracker_.sample(dt,{contacts.data(),count},settings);
            if(settings.damageActors)for(std::size_t i=0;i<result.count;++i){
                for(std::size_t j=0;j<count;++j)if(contacts[j].id==result.hits[i].id){
                    const auto ref=RE::TESHavokUtilities::FindCollidableRef(*bodies[j]);
                    auto actor=ref?ref->As<RE::Actor>():nullptr;
                    if(actor&&actor!=player)actors[i].reset(actor);
                    break;
                }
            }
        }
        if(!result.count)return false;
        // Multiple collision shapes on one actor cannot multiply its damage.
        for(std::size_t i=0;i<result.count;++i)if(actors[i]&&!actors[i]->IsDead()){
            bool seen=false;for(std::size_t j=0;j<i;++j)if(actors[j].get()==actors[i].get())seen=true;
            if(seen)continue;
            float damage=result.hits[i].damage;
            for(std::size_t j=i+1;j<result.count;++j)if(actors[j].get()==actors[i].get())damage=std::max(damage,result.hits[j].damage);
            const bool applied=actors[i]->DoDamage(damage,player,false);
            SKSE::log::debug("Flight collision actor: target={:08X} requestedDamage={:.2f} nativeResult={}",actors[i]->GetFormID(),damage,applied);
        }
        if(!player->IsDead()){
            const float before=player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
            const bool applied=player->DoDamage(result.playerDamage,nullptr,false);
            SKSE::log::debug("Flight collision player: contacts={} requestedDamage={:.2f} multiplier={:.2f} threshold={:.2f} healthBefore={:.2f} healthAfter={:.2f} nativeResult={}",
                result.count,result.playerDamage,settings.multiplier,settings.minSpeed,before,player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth),applied);
        }
        playFlightCollisionSound(player);
        return true;
    }
private:
    impact::Tracker tracker_;
    RE::bhkCharacterController* controller_{}; // identity, reacquired on each update
};
}
