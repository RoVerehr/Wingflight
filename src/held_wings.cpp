#include "held_wings.hpp"
#include "held_wing_policy.hpp"
#include "effect_owner.hpp"
#include "runtime_support.hpp"
#include "diagnostics.hpp"
#include <RE/H/hkbContext.h>
#include <RE/H/hkbBehaviorGraphData.h>
#include <RE/H/hkbBehaviorGraphStringData.h>
#include <SKSE/SKSE.h>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace wingflight {
namespace {
struct Entry {
    RE::NiPointer<RE::ModelReferenceEffect> model;
    std::array<RE::BSTSmartPointer<RE::BShkbAnimationGraph>,8> graphs{};
    bool managed{},open{};float retry{};unsigned attempts{};
};
std::mutex eventMutex;
std::unordered_map<RE::ModelReferenceEffect*,Entry> entries;
HeldWingIntent intent;
bool installed{};
std::atomic_bool anyManaged{};
unsigned samples{};
struct WingEvents {
    // Live 1170 AD8570 reads the event ID from [R8] then tail-calls dispatch.
    // Preserve this borrowed event pointer; do not copy/refcount its payload.
    static void thunk(RE::hkbStateMachine* self,const RE::hkbContext& context,const RE::hkbEvent* event){
        if(!anyManaged.load(std::memory_order_acquire)){original(self,context,event);return;}
        bool managed=false,open=false;
        if(event&&self->name.c_str()&&std::string_view(self->name.c_str())=="HandCastingFXBehavior"){
            std::lock_guard lock(eventMutex);
            for(const auto& [model,entry]:entries)for(const auto& graph:entry.graphs)
                if(graph && &graph->characterInstance==context.character){managed=entry.managed;open=entry.open;break;}
        }
        if(managed&&context.behavior&&context.behavior->data&&context.behavior->data->stringData){
            const auto& names=context.behavior->data->stringData->eventNames;
            const auto map=static_cast<const RE::hkbSymbolIdMap*>(context.behavior->eventIDMap.get());
            const auto external=static_cast<std::int32_t>(event->id);
            if(names.size()<=256)for(std::int32_t i=0;i<names.size();++i){
                const auto id=map?(i<map->internalToExternalMap.size()?map->internalToExternalMap[i]:-1):i;
                if(id!=external||!names[i].c_str())continue;
                if(heldWingBlocks(names[i].c_str(),true,open))return;
                break;
            }
        }
        original(self,context,event);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
int wingState(RE::ModelReferenceEffect* model){
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if(!model->GetAnimationGraphManager(manager)||!manager)return -1;
    const auto stateTable=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
    for(const auto& graph:manager->graphs){
        if(!graph||graph->projectName!="AnimatedDragonWings")continue;
        const auto behavior=graph->characterInstance.behaviorGraph;
        if(!behavior||!behavior->activeNodes||behavior->activeNodes->size()>128)continue;
        for(const auto& info:*behavior->activeNodes){
            auto node=info.nodeClone;
            if(!node||*reinterpret_cast<std::uintptr_t*>(node)!=stateTable)continue;
            const auto state=static_cast<const RE::hkbStateMachine*>(node);
            if(state->isActive&&state->name.c_str()&&std::string_view(state->name.c_str())=="HandCastingFXBehavior")return state->currentStateID;
        }
    }
    return -1;
}
}
void installHeldWingEvents(){
    if(installed)return;
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_hkbStateMachine[0]};
    const auto site=table.address()+6*sizeof(std::uintptr_t);
    if(!runtimeReadable(reinterpret_cast<void*>(site),sizeof(std::uintptr_t))){
        diagnostic::report("held-wings.event-slot-unreadable","leave original wing events unchanged");return;
    }
    const auto destination=*reinterpret_cast<std::uintptr_t*>(site);
    if(!runtimeExecutable(destination)||destination==reinterpret_cast<std::uintptr_t>(&WingEvents::thunk)){
        diagnostic::report("held-wings.event-target-invalid","leave original wing events unchanged");return;
    }
    // Chain to any existing callable hook, preserving the compatibility policy.
    WingEvents::original=table.write_vfunc(6,WingEvents::thunk);installed=true;
    SKSE::log::info("AWU held-jump wings revision=held-jump-1: player dragon/feather ART events only, original target={:X}",destination);
}
void resetHeldWings(){anyManaged.store(false,std::memory_order_release);std::lock_guard lock(eventMutex);entries.clear();intent.reset();samples=0;}
bool heldWingGateInstalled(){return installed;}
void updateHeldWings(WingMatch match,float dt,bool held,bool airborne,bool fullFlight,bool glide,bool suspended,float threshold){
    if(!installed||!heldWingsEligible(match.profile)||match.owner!=RE::PlayerCharacter::GetSingleton()
        ||!wingEventActor(match.owner,match.effect)||!match.effect->hitEffects){resetHeldWings();return;}
    const bool open=intent.update(dt,held,airborne,fullFlight,glide,suspended,threshold);
    // During menu suspension keep the last gate and do not issue new commands.
    if(suspended)return;
    const bool managed=!fullFlight;
    std::array<RE::NiPointer<RE::ModelReferenceEffect>,16> models{};unsigned count=0;
    for(auto effect:*match.effect->hitEffects){
        if(!effect||effect->finished||effect->GetType()!=RE::TEMP_EFFECT_TYPE::kRefModel)continue;
        auto model=static_cast<RE::ModelReferenceEffect*>(effect);
        if(!model->GetAttached()||!model->Get3D())continue;
        if(count==models.size()){diagnostic::report("held-wings.model-limit","manage first 16 wing models",match.owner);break;}
        models[count++].reset(model);
    }
    {
        std::lock_guard lock(eventMutex);
        std::erase_if(entries,[&](const auto& pair){for(unsigned i=0;i<count;++i)if(pair.second.model.get()==models[i].get())return false;return true;});
        for(unsigned i=0;i<count;++i){
            auto& entry=entries[models[i].get()];
            if(!entry.model)entry.model=models[i];
            if(entry.managed!=managed||entry.open!=open){entry.retry=0;entry.attempts=0;}
            entry.managed=managed;entry.open=open;
            entry.retry=std::max(0.f,entry.retry-std::clamp(dt,0.f,.1f));
        }
    }
    // Keep graph owners alive while their character identities are registered.
    // Collect before graph evaluation so the first jump event is already gated.
    for(unsigned i=0;i<count;++i){
        std::array<RE::BSTSmartPointer<RE::BShkbAnimationGraph>,8> graphs{};
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(models[i]->GetAnimationGraphManager(manager)&&manager){
            RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);unsigned n=0;
            for(const auto& graph:manager->graphs)if(graph&&graph->projectName=="AnimatedDragonWings"&&n<graphs.size())graphs[n++]=graph;
        }
        std::lock_guard lock(eventMutex);entries.at(models[i].get()).graphs=std::move(graphs);
    }
    anyManaged.store(managed&&count>0,std::memory_order_release);
    if(!managed)return;
    for(unsigned i=0;i<count;++i){
        const auto model=models[i].get();const int state=wingState(model);
        const auto command=heldWingCommand(state,open);
        bool send=false;{
            std::lock_guard lock(eventMutex);auto& entry=entries.at(model);
            if(!command){if((open&&state==8)||(!open&&state==2))entry.attempts=0;continue;}
            if(entry.retry==0&&entry.attempts<4){send=true;entry.retry=.15f;++entry.attempts;}
            else if(entry.retry==0&&entry.attempts>=4)diagnostic::report("held-wings.transition-not-observed","stop retries until held state changes",model,command,state);
        }
        if(send){
            // JumpFall is permitted only when open is requested; fold commands
            // are permitted only on release. Queued events obey the same gate.
            const bool accepted=model->NotifyAnimationGraph(RE::BSFixedString(command));
            if(samples++<20||!accepted)SKSE::log::info("AWU held-jump wing command: open={} state={} event={} accepted={} held={}",open,state,command,accepted,held);
            if(!accepted)diagnostic::report("held-wings.command-rejected","retain native wing pose",model,command);
        }
    }
}
}
