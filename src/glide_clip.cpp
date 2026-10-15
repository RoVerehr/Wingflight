#include "glide_clip.hpp"
#include "logging.hpp"
#include "glide_clip_policy.hpp"
#include "lower_body.hpp"
#include "armed_sprint_clip.hpp"
#include "flight_pose.hpp"
#include "wings.hpp"
#include "continuous_wing_rate.hpp"
#include "actor_flight.hpp"
#include <SKSE/SKSE.h>
#include <array>
#include <atomic>
namespace wingflight {
namespace {
std::array<std::atomic<const RE::hkbCharacter*>,8> characters{};
std::atomic_uint samples{};
std::atomic_uint npcSamples{};
struct ClipUpdate {
    static void thunk(RE::hkbClipGenerator* clip,const RE::hkbContext& context,float dt) {
        updateArmedSprintIdle(clip,context);
        updateFlightPose(clip,context);
        bool selected=false;
        if(context.character) for(auto& character:characters) if(character.load()==context.character){selected=true;break;}
        const bool npcFalling=actorFallClipGuard(context.character);selected=selected||npcFalling;
        if(selected && clip && clip->name.c_str() && timedFallClip(clip->name.c_str())
            && clip->mode==RE::hkbClipGenerator::kModeSinglePlay && clip->binding && clip->binding->animation
            && clip->triggers && context.behavior && context.behavior->data && context.behavior->data->stringData) {
            const auto& names=context.behavior->data->stringData->eventNames;
            for(const auto& trigger:clip->triggers->triggers) {
                const auto id=trigger.event.id;
                if(!trigger.relativeToEndOfClip || trigger.isAnnotation || std::abs(trigger.localTime+0.1f)>0.001f
                    || static_cast<std::uint32_t>(id)>=static_cast<std::uint32_t>(names.size())) continue;
                if(!names[id].c_str())continue;
                const std::string_view event=names[id].c_str();
                if(event!="JumpLand" && event!="JumpLandDirectional") continue;
                const float duration=clip->binding->animation->duration-clip->cropStartAmountLocalTime-clip->cropEndAmountLocalTime;
                const float step=glideClipStep(dt,clip->localTime,duration,clip->playbackSpeed,trigger.localTime);
                if(logging::detailed()&&step<dt && (npcFalling?npcSamples:samples).fetch_add(1)<12)
                    SKSE::log::debug("{}: native fall clip held before timer; clip={} time={:.3f} duration={:.3f} step={:.4f}; physical landing remains native",npcFalling?"NPC descent":"Held glide",clip->name.c_str(),clip->localTime,duration,step);
                dt=step;
                break;
            }
        }
        // Preserve the installed OAR/AMR hook chain, including replacement checks.
        const float wingRate=continuousWingClipRate(clip,context);
        const float before=wingRate>0?clip->localTime:0;
        {
            ScopedContinuousWingRate rate(clip,wingRate);
            original(clip,context,dt);
        }
        observeContinuousWingClip(clip,before,dt,wingRate);
        observeLowerBodyClip(clip,context);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct ClipDeactivate {
    static void thunk(RE::hkbClipGenerator* clip,const RE::hkbContext& context){
        forgetLowerBodyClip(clip);original(clip,context);afterArmedSprintDeactivate(clip,context);afterFlightPoseDeactivate(clip,context);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct ClipActivate {
    static void thunk(RE::hkbClipGenerator* clip,const RE::hkbContext& context){
        beforeFlightPoseActivate(clip,context);beforeArmedSprintActivate(clip,context);original(clip,context);afterArmedSprintActivate(clip,context);afterFlightPoseActivate(clip,context);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
}
void setGlideClipGuard(RE::PlayerCharacter* player,bool enabled) {
    if(!enabled || !player){for(auto& character:characters)character=nullptr;return;}
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if(!player->GetAnimationGraphManager(manager)||!manager){for(auto& character:characters)character=nullptr;return;}
    RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
    std::size_t i=0;
    for(const auto& graph:manager->graphs) if(graph && graph->holder==player && i<characters.size()) characters[i++]=&graph->characterInstance;
    while(i<characters.size())characters[i++]=nullptr;
}
void installGlideClipGuard(){
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_hkbClipGenerator[0]};
    ClipActivate::original=table.write_vfunc(4,ClipActivate::thunk);
    ClipUpdate::original=table.write_vfunc(5,ClipUpdate::thunk);
    ClipDeactivate::original=table.write_vfunc(7,ClipDeactivate::thunk);
}
}
