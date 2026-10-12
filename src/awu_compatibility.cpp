#include "awu_compatibility.hpp"
#include <atomic>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include "diagnostics.hpp"

namespace wingflight {
namespace {
constexpr const char* plugin="Animated Wings Ultimate.esp";
constexpr const char* script="AFW_MagicEffect";
std::atomic_uint64_t session{0};
bool active{};
RE::TESGlobal* awuGlideDisabled{};
struct EffectState { bool ready{},pending{},swimPending{},suspended{};unsigned retry{};
    std::chrono::steady_clock::time_point diagnosticWait{}; };
std::unordered_map<RE::VMHandle,EffectState> effects;
RE::BSScript::Internal::VirtualMachine* vm(){return RE::BSScript::Internal::VirtualMachine::GetSingleton();}
RE::VMHandle handle(RE::ActiveEffect* effect){return vm()->GetObjectHandlePolicy()->GetHandleForObject(RE::ActiveEffect::VMTYPEID,effect);}
bool bound(RE::ActiveEffect* effect,RE::BSTSmartPointer<RE::BSScript::Object>& object){
    if(!effect || !effect->GetBaseObject())return false;
    const auto file=effect->GetBaseObject()->GetFile(0);
    return file && std::string_view(file->fileName)==plugin && vm()->FindBoundObject(handle(effect),script,object);
}
bool boolean(RE::BSScript::Object& object,const char* name){
    const auto value=object.GetVariable(name);return value && value->IsBool() && value->GetBool();
}
bool initialized(RE::BSScript::Object& object){
    // AWU assigns this AFTER registering Jump, so a delayed OnEffectStart cannot
    // re-register the control after our helper has finished unregistering it.
    const auto value=object.GetVariable("_PreEffectSwimSpeed");
    return value && value->IsFloat() && value->GetFloat()>0;
}
class Completed final : public RE::BSScript::IStackCallbackFunctor {
public:
    Completed(RE::VMHandle value,bool swim,bool suspend=false):effect(value),generation(session.load()),swimming(swim),suspending(suspend){}
    void operator()(RE::BSScript::Variable result) override {
        const bool accepted=swimming || (result.IsBool() && result.GetBool());
        const auto id=effect,epoch=generation;const auto isSwim=swimming;
        const auto suspend=suspending;
        SKSE::GetTaskInterface()->AddTask([id,epoch,isSwim,accepted,suspend]{
            if(epoch!=session.load())return;
            const auto found=effects.find(id);if(found==effects.end())return;
            if(isSwim){found->second.swimPending=false;return;}
            found->second.pending=false;found->second.ready=accepted;
            if(accepted)found->second.suspended=suspend;
            found->second.retry=accepted?0:30;
            SKSE::log::info("AWU independent input handoff completed: effect={} accepted={} suspended={}; jump/damage settings untouched",id,accepted,suspend);
        });
    }
    void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override{}
private:
    RE::VMHandle effect;std::uint64_t generation;bool swimming,suspending;
};
}
void initializeAWUCompatibility(bool enabled){
    active=false;awuGlideDisabled=nullptr;
    const auto data=RE::TESDataHandler::GetSingleton();
    if(!enabled || !(data->LookupLoadedModByName(plugin) || data->LookupLoadedLightModByName(plugin))){
        SKSE::log::info("AWU compatibility inactive: enabled={} plugin present={}",enabled,
            bool(data->LookupLoadedModByName(plugin) || data->LookupLoadedLightModByName(plugin)));return;
    }
    if(!std::filesystem::is_regular_file("Data/Scripts/WingflightAWUCompatibility.pex")){
        SKSE::log::warn("AWU detected but optional WingflightAWUCompatibility.pex is missing; AWU flight adapter will remain inactive");return;
    }
    awuGlideDisabled=data->LookupForm<RE::TESGlobal>(0x867,plugin);
    if(!awuGlideDisabled || std::string_view(awuGlideDisabled->GetFormEditorID())!="AFW_GlideEffectDisabled"){
        awuGlideDisabled=nullptr;SKSE::log::warn("Unrecognized AWU glide option; compatibility inactive");return;
    }
    active=true;
    SKSE::log::info("AWU independent compatibility active; original AFW scripts retained; no jump/damage/global-physics settings are changed");
}
void resetAWUCompatibility(){++session;effects.clear();}
void updateAWUCompatibility(RE::PlayerCharacter* player,bool ownGlide){
    if(!active || !player || !vm())return;
    const auto list=player->AsMagicTarget()->GetActiveEffectList();
    std::unordered_set<RE::VMHandle> current;
    if(list)for(auto effect:*list){
        if(!effect || effect->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled))continue;
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if(!bound(effect,object))continue;
        const auto id=handle(effect);current.insert(id);auto& state=effects[id];
        if(!state.ready){
            const auto now=std::chrono::steady_clock::now();
            if(state.diagnosticWait==std::chrono::steady_clock::time_point{})state.diagnosticWait=now;
            const auto wait=std::chrono::duration<float>(now-state.diagnosticWait).count();
            if(wait>5)diagnostic::report(state.pending?"awu.input-helper-pending":"awu.effect-initialization-pending",
                "keep adapter ineligible and retain existing retry policy",effect,script,wait,5);
        }else state.diagnosticWait={};
        if(state.pending || (state.ready && state.suspended==ownGlide
            && !(ownGlide && boolean(*object,"_IsGliding"))))continue;
        if(state.retry>0){--state.retry;continue;}
        if(!initialized(*object))continue;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new Completed(id,false,ownGlide));
        if(ownGlide)state.pending=vm()->DispatchStaticCall("WingflightAWUCompatibility","SuspendGlide",
            RE::MakeFunctionArguments(static_cast<RE::ActiveEffect*>(effect),boolean(*object,"_IsGliding")),callback);
        else state.pending=vm()->DispatchStaticCall("WingflightAWUCompatibility","ResumeGlide",
            RE::MakeFunctionArguments(static_cast<RE::ActiveEffect*>(effect),awuGlideDisabled->value==0),callback);
        if(!state.pending){state.retry=30;SKSE::log::warn("Could not dispatch independent AWU input helper for effect={}",id);}
    }
    std::erase_if(effects,[&](const auto& item){return !current.contains(item.first);});
}
bool awuEffectReady(RE::Actor* owner,RE::ActiveEffect* effect){
    if(!active || owner!=RE::PlayerCharacter::GetSingleton() || !effect || !vm())return false;
    const auto found=effects.find(handle(effect));return found!=effects.end() && found->second.ready;
}
void awuAnimationEvent(RE::PlayerCharacter* player,const RE::BSFixedString& event){
    if(!active || !player || (event!="TailCombatIdle" && event!="TailCombatLocomotion"))return;
    const auto list=player->AsMagicTarget()->GetActiveEffectList();if(!list)return;
    for(auto effect:*list){
        RE::BSTSmartPointer<RE::BSScript::Object> object;
        if(!bound(effect,object) || !boolean(*object,"_IsSwimming"))continue;
        const auto found=effects.find(handle(effect));if(found==effects.end() || found->second.swimPending)continue;
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new Completed(found->first,true));
        // Ask the unchanged AWU script to perform its OWN swimming cleanup.
        // This also preserves NPC scanning and ancillary wing effects.
        found->second.swimPending=vm()->DispatchMethodCall(object,"OnAnimationEvent",
            RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(player),RE::BSFixedString("MTState")),callback);
    }
}
}
