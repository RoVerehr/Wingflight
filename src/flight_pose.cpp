#include "flight_pose.hpp"
#include "flight_pose_policy.hpp"
#include "body_callsite.hpp"
#include "runtime_support.hpp"
#include "runtime_callable.hpp"
#include "lower_body_policy.hpp"
#include "OpenAnimationReplacerAPI-Animations.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <MinHook.h>

namespace wingflight {
namespace {
FlightPoseGate gate{};
std::array<std::string_view,3> defaultPaths{};
std::array<std::string_view,3> fallbackPaths{};
std::atomic_bool loaderInstalled{};
SKSE::Trampoline poseTrampoline{"Wingflight flight pose loader"};
std::array<std::atomic<const RE::hkbCharacter*>,8> characters{};
std::atomic_uint samples{};
std::atomic_uint sharedBindingSamples{};
std::atomic_uint bindingRepairSamples{};
std::atomic_uint activatedPoseSamples{};
OAR_API::Animations::IAnimationsInterface* oar{};
using Loader=void(*)(RE::hkbCharacterStringData*,RE::hkbAnimationBindingSet*,void*,RE::hkbBehaviorGraph*,const char*,RE::BSTHashMap<RE::BSFixedString,std::uint32_t>*);
Loader originalLoader{},nativeLoader{};
struct Registered {
    RE::hkRefPtr<RE::hkbCharacterStringData> data;
    std::array<std::uint16_t,3> indices{};
};
std::mutex registrationLock;
std::unordered_map<RE::hkbCharacterStringData*,Registered> registrations;
RE::hkbCharacterStringData* strings(const RE::hkbContext& c){return c.character&&c.character->setup&&c.character->setup->data?c.character->setup->data->stringData.get():nullptr;}
std::uint16_t slotIndex(RE::hkbCharacterStringData* data,unsigned mode=0){
    std::lock_guard guard(registrationLock);
    const auto it=registrations.find(data);
    return it!=registrations.end()&&mode<3?it->second.indices[mode]:0xffff;
}
bool selected(const RE::hkbContext& c){if(c.character)for(auto& p:characters)if(p.load()==c.character)return true;return false;}
bool eligible(const RE::hkbContext& c){return selected(c)&&gate&&gate(RE::PlayerCharacter::GetSingleton());}
pose::TransitionTiming transitionTiming;
// Owned here so corresponding source does not depend on private CommonLib
// additions. Havok 2010's blending transition layout is the same in SE/AE.
struct FlightTransition : RE::hkbGenerator {
    std::uint8_t transitionOptions[8];
    float duration,toGeneratorStartTimeFraction;
    std::uint16_t flags;
    std::uint8_t endMode,blendCurve;
    std::uint16_t alignmentBone,padding;
    RE::hkbGenerator *fromGenerator,*toGenerator;
    RE::hkArray<RE::hkQsTransform> worldTransforms;
    float timeRemaining,timeInTransition;
};
static_assert(offsetof(FlightTransition,duration)==0x50);
static_assert(offsetof(FlightTransition,fromGenerator)==0x60);
static_assert(offsetof(FlightTransition,timeRemaining)==0x80);
struct TransitionRecord {const RE::hkbCharacter* character{};float original{},seconds{};};
std::mutex transitionLock;
std::unordered_map<FlightTransition*,TransitionRecord> transitions;
std::atomic_uint transitionSamples{};
std::atomic_uint transitionSkipSamples{};
std::string_view nodeName(const RE::hkbNode* node){return node&&node->name.c_str()?node->name.c_str():"";}
struct NativeTransitionActivate {
    static void thunk(FlightTransition* effect,const RE::hkbContext& context){
        // Transition templates can be shared between characters. Only edit the
        // player's runtime clone, and never change on-disk behavior resources.
        if(effect&&eligible(context)){
            const float seconds=pose::transitionSeconds(nodeName(effect),nodeName(effect->fromGenerator),nodeName(effect->toGenerator),transitionTiming);
            if(seconds>0&&effect->cloneState==RE::hkbNode::CloneState::kClone&&std::isfinite(effect->duration)&&effect->duration>=0){
                std::lock_guard guard(transitionLock);
                if(transitions.size()<256){
                    transitions[effect]={context.character,effect->duration,seconds};
                    if(transitionSamples.fetch_add(1)<32)SKSE::log::info("Flight native blend: effect={} from={} to={} seconds={:.3f}->{:.3f}",nodeName(effect),nodeName(effect->fromGenerator),nodeName(effect->toGenerator),effect->duration,seconds);
                    if(effect->duration>0&&effect->timeRemaining>0){
                        const float ratio=seconds/effect->duration;
                        effect->timeRemaining*=ratio;effect->timeInTransition*=ratio;
                    }
                    effect->duration=seconds;
                }else SKSE::log::warn("Flight native blend skipped: transition tracking capacity reached");
            }else if((seconds>0||nodeName(effect)=="SprintSyncBlendTransition")&&transitionSkipSamples.fetch_add(1)<12)
                SKSE::log::warn("Flight native blend kept original timing: effect={} from={} to={} clone={} duration={}; expected player runtime clone and recognized native locomotion pair",nodeName(effect),nodeName(effect->fromGenerator),nodeName(effect->toGenerator),static_cast<unsigned>(effect->cloneState.get()),effect->duration);
        }
        original(effect,context);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct NativeTransitionUpdate {
    static void thunk(FlightTransition* effect,const RE::hkbContext& context,float dt){
        const bool wanted=eligible(context);
        {
            std::lock_guard guard(transitionLock);
            if(auto it=transitions.find(effect);it!=transitions.end()){
                if(it->second.character!=context.character)transitions.erase(it);
                else if(wanted){
                    // The graph's variable binding may write duration each frame.
                    effect->duration=it->second.seconds;
                }else{
                    // Release on landing/drawing/exit without stretching a later
                    // non-flight transition. Preserve native normalized progress.
                    const float ratio=it->second.original/it->second.seconds;
                    effect->duration=it->second.original;
                    effect->timeRemaining*=ratio;effect->timeInTransition*=ratio;
                    transitions.erase(it);
                }
            }
        }
        original(effect,context,dt);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct NativeTransitionDeactivate {
    static void thunk(FlightTransition* effect,const RE::hkbContext& context){
        original(effect,context);
        std::lock_guard guard(transitionLock);
        if(auto it=transitions.find(effect);it!=transitions.end()){
            if(it->second.character==context.character)effect->duration=it->second.original;
            transitions.erase(it);
        }
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
void installNativeTransitions(){
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_hkbBlendingTransitionEffect[0]};
    if(!runtimeReadable(reinterpret_cast<void*>(table.address()),8*8)){
        SKSE::log::error("Flight native blend hook unavailable: unreadable vtable; native timings retained");return;
    }
    for(auto slot:{4u,5u,7u}){
        const auto address=*reinterpret_cast<const std::uintptr_t*>(table.address()+8*slot);
        if(!runtime::executableMemory(address)){
            SKSE::log::error("Flight native blend hook unavailable: slot={} target={:X} is not callable; native timings retained",slot,address);return;
        }
    }
    NativeTransitionActivate::original=table.write_vfunc(4,NativeTransitionActivate::thunk);
    NativeTransitionUpdate::original=table.write_vfunc(5,NativeTransitionUpdate::thunk);
    NativeTransitionDeactivate::original=table.write_vfunc(7,NativeTransitionDeactivate::thunk);
    SKSE::log::info("Flight native transition hooks installed: player unarmed flight clones only; existing hooks chained");
}
void load(RE::hkbCharacterStringData* data,RE::hkbAnimationBindingSet* bindings,void* assets,RE::hkbBehaviorGraph* graph,const char* path,RE::BSTHashMap<RE::BSFixedString,std::uint32_t>* events){
    if(data&&path&&pose::project(legs::canonical(path))){
        std::lock_guard guard(registrationLock);
        auto it=registrations.find(data);
        if(it==registrations.end()){
            if(!pose::canAppend(data->animationNames.size()+2))SKSE::log::error("Flight pose registration failed: project={} animationCount={} exceeds three-slot limit",path,data->animationNames.size());
            else{
                Registered record;record.data=RE::hkRefPtr<RE::hkbCharacterStringData>{data};
                for(unsigned mode=0;mode<3;++mode){
                    record.indices[mode]=static_cast<std::uint16_t>(data->animationNames.size());
                    data->animationNames.push_back(pose::slots[mode].data());
                    SKSE::log::info("Single-file flight pose registered before OAR: project={} slot={} name={}",path,record.indices[mode],pose::slots[mode]);
                }
                it=registrations.emplace(data,std::move(record)).first;
            }
        }else{
            // Repeated project loads expose the logical names to OAR again.
            for(unsigned mode=0;mode<3;++mode)data->animationNames[it->second.indices[mode]]=pose::slots[mode].data();
        }
    }
    originalLoader(data,bindings,assets,graph,path,events);
}
void loadDefaults(RE::hkbCharacterStringData* data,RE::hkbAnimationBindingSet* bindings,void* assets,RE::hkbBehaviorGraph* graph,const char* path,RE::BSTHashMap<RE::BSFixedString,std::uint32_t>* events){
    {
        std::lock_guard guard(registrationLock);
        if(auto it=registrations.find(data);it!=registrations.end()){
            // OAR has registered conditional custom files against these indices.
            // Keep FAR as the base so a custom file's conditions are respected;
            // loading that custom file as the base would bypass its conditions.
            for(unsigned mode=0;mode<3;++mode){
                const auto index=it->second.indices[mode];
                if(index<data->animationNames.size())data->animationNames[index]=fallbackPaths[mode].data();
            }
        }
    }
    nativeLoader(data,bindings,assets,graph,path,events);
}
struct Saved {
    const RE::hkbCharacter* character{};
    RE::hkRefPtr<RE::hkbCharacterStringData> data;
    std::string node,path;
    RE::hkStringPtr name;
    std::uint16_t native{},pose{};
    unsigned modeIndex{};
    float cropStart{},cropEnd{},start{},speed{},duration{};
    RE::hkbClipGenerator::PlaybackMode mode{};
    RE::hkRefPtr<RE::hkbClipTriggerArray> triggers,originalTriggers;
};
std::mutex lock;
std::unordered_map<RE::hkbClipGenerator*,Saved> saved;
bool same(const Saved& s,RE::hkbClipGenerator* p,const RE::hkbContext& c){return s.character==c.character&&s.data.get()==strings(c)&&p->name.c_str()&&s.node==p->name.c_str();}
bool graphOwnsClip(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    const auto contains=[p](RE::hkbBehaviorGraph* graph){
        if(!graph||!graph->activeNodes||graph->activeNodes->size()>4096)return false;
        for(const auto& node:*graph->activeNodes)if(node.nodeClone==p||node.nodeTemplate==p)return true;
        return false;
    };
    return contains(c.behavior)||(c.character&&contains(c.character->behaviorGraph.get()));
}
bool outgoingCopy(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    if(p->userData!=0xc)return false;
    bool tracked{};
    {std::lock_guard guard(lock);const auto it=saved.find(p);tracked=it!=saved.end()&&same(it->second,p,c);}
    return pose::ignoreTransitionCopy(true,tracked,graphOwnsClip(p,c));
}
void restore(RE::hkbClipGenerator* p,const Saved& s){
    p->animationName=s.name;p->animationBindingIndex=s.native;
    p->cropStartAmountLocalTime=s.cropStart;p->cropEndAmountLocalTime=s.cropEnd;p->startTime=s.start;p->playbackSpeed=s.speed;p->enforcedDuration=s.duration;p->mode=s.mode;
    p->triggers=s.triggers;p->originalTriggers=s.originalTriggers;
}
std::string playing(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    auto data=strings(c);auto i=p->animationBindingIndex;
    return data&&i<data->animationNames.size()&&data->animationNames[i].c_str()?legs::canonical(data->animationNames[i].c_str()):std::string{};
}
bool currentPoseBinding(RE::hkbClipGenerator* p,const RE::hkbContext& c,unsigned mode){
    const auto physical=playing(p,c);
    const auto info=oar?oar->GetCurrentReplacementAnimationInfo(p):OAR_API::Animations::ReplacementAnimationInfo{};
    const auto logical=info.animationPath.c_str()?legs::canonical(info.animationPath.c_str()):std::string{};
    bool accepted=pose::acceptedBinding(physical,logical,mode);
    if(logical.empty()&&p->animationBindingIndex==slotIndex(strings(c),mode)){
        accepted=true;
        // OAR can restore an index after a transition while the loaded binding
        // still points to the previous motion. Check the native slot's asset,
        // without assigning/aliasing any engine-owned binding pointers.
        const auto set=c.character?c.character->animationBindingSet.get():nullptr;
        const auto index=p->animationBindingIndex;
        if(set&&index<set->bindings.size()){
            const auto entry=set->bindings[index];RE::hkaAnimationBinding* expected{};
            // hkbAnimationBindingWithTriggers: hkReferencedObject header (0x10),
            // binding at 0x10. Read-only verified from live 1.6.1170 bindings.
            if(entry&&runtimeReadable(entry,0x18))std::memcpy(&expected,reinterpret_cast<const std::byte*>(entry)+0x10,sizeof(expected));
            if(expected&&runtimeReadable(expected,sizeof(RE::hkaAnimationBinding))&&p->binding&&expected->animation)
                accepted=p->binding->animation.get()==expected->animation.get();
            if(!accepted&&bindingRepairSamples.fetch_add(1)<16)
                SKSE::log::warn("Flight pose cached binding differs: slot={} index={} bound={:X} expected={:X}; reactivate through native/OAR chain",pose::slots[mode],index,reinterpret_cast<std::uintptr_t>(p->binding),reinterpret_cast<std::uintptr_t>(expected));
        }
    }
    if(accepted&&sharedBindingSamples.fetch_add(1)<16)
        SKSE::log::info("Flight pose OAR shared binding retained: index={} physical={} logical={} (deduplicated animation is valid)",p->animationBindingIndex,physical,logical);
    return accepted;
}
}
void initializeFlightPose(FlightPoseGate g,FlightPoseMode){gate=g;}
void configureFlightPoseTransitions(pose::TransitionTiming timing){transitionTiming=timing;}
bool flightPoseReady(){return loaderInstalled.load();}
void installFlightPoseLoader(){
    if(loaderInstalled)return;
    const auto module=GetModuleHandleW(L"OpenAnimationReplacer.dll");
    const auto request=module?reinterpret_cast<OAR_API::Animations::_RequestPluginAPI_Animations>(GetProcAddress(module,"RequestPluginAPI_Animations")):nullptr;
    if(request)oar=request(OAR_API::Animations::InterfaceVersion::V1,"Wingflight",REL::Version(0,3,14,0));
    const auto exists=[](std::string_view path){
        std::error_code error;
        const auto file=std::string("Data/meshes/actors/character/")+std::string(path);
        const bool present=std::filesystem::is_regular_file(file,error);
        if(error&&error!=std::errc::no_such_file_or_directory&&error!=std::errc::not_a_directory)
            SKSE::log::warn("Flight pose file lookup failed: path={} error={}",file,error.message());
        return present;
    };
    defaultPaths=pose::resolveDefaults(exists);
    fallbackPaths=pose::resolveDefaults([&](std::string_view path){
        for(auto local:pose::defaults)if(path==local)return false;
        return exists(path);
    });
    bool missing{};
    for(unsigned mode=0;mode<2;++mode)if(fallbackPaths[mode].empty()){
        SKSE::log::error("Flight pose loader disabled: slot={} required FAR resource={} missing; install and enable Flying Animations - Resource",pose::slots[mode],pose::resources[mode]);
        missing=true;
    }
    if(missing)return;
    for(unsigned mode=0;mode<3;++mode)
        SKSE::log::info("Flight pose resource resolved: slot={} localCandidate={} base={}; OAR controls local conditions",pose::slots[mode],defaultPaths[mode]==pose::defaults[mode]?defaultPaths[mode]:"none",fallbackPaths[mode]);
    if(!oar){SKSE::log::error("Single-file flight pose loader disabled: OAR animation API unavailable");return;}
    const bool se=runtimeBackend()==runtime::Backend::steam1597;
    const auto site=REL::RelocationID(63028,63846).address()+(se?0x8d:0x101);
    if(!runtimeReadable(reinterpret_cast<void*>(site),6)){SKSE::log::error("Flight pose loader: unreadable call site runtime={} address={:X}",runtimeName(),site);return;}
    const auto call=runtime::inspectSceneCall(site,{reinterpret_cast<const std::uint8_t*>(site),6});
    if(!call.size){SKSE::log::error("Flight pose loader: unsupported call runtime={} site={:X} firstByte={:02X}",runtimeName(),site,*reinterpret_cast<const std::uint8_t*>(site));return;}
    auto destination=call.target;
    if(call.indirect){if(!runtimeReadable(reinterpret_cast<void*>(destination),8))return;std::memcpy(&destination,reinterpret_cast<void*>(destination),8);}
    if(!runtime::executableMemory(destination)||destination==reinterpret_cast<std::uintptr_t>(&load)){
        SKSE::log::error("Flight pose loader: target not callable or recursive; site={:X} target={:X}",site,destination);return;
    }
    const auto nativeEntry=REL::RelocationID(63032,63950).address();
    if(!runtime::executableMemory(nativeEntry)){SKSE::log::error("Flight pose Default loader disabled: native entry not callable runtime={} address={:X}",runtimeName(),nativeEntry);return;}
    const auto init=MH_Initialize();
    if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED){SKSE::log::error("Flight pose Default loader MinHook initialization failed: {}",static_cast<int>(init));return;}
    auto status=MH_CreateHook(reinterpret_cast<void*>(nativeEntry),reinterpret_cast<void*>(&loadDefaults),reinterpret_cast<void**>(&nativeLoader));
    if(status!=MH_OK){SKSE::log::error("Flight pose Default loader hook creation failed: status={} runtime={} entry={:X}",static_cast<int>(status),runtimeName(),nativeEntry);return;}
    status=MH_EnableHook(reinterpret_cast<void*>(nativeEntry));
    if(status!=MH_OK){MH_RemoveHook(reinterpret_cast<void*>(nativeEntry));SKSE::log::error("Flight pose Default loader hook enabling failed: {}",static_cast<int>(status));return;}
    originalLoader=reinterpret_cast<Loader>(destination);
    poseTrampoline.create(14);auto& trampoline=poseTrampoline;
    if(call.size==5)trampoline.write_call<5>(site,load);else trampoline.write_call<6>(site,load);
    loaderInstalled=true;
    installNativeTransitions();
    SKSE::log::info("Single-file flight pose loader installed: runtime={} site={:X} chainedTarget={:X}; private trampoline; OAR keeps conditional selection",runtimeName(),site,destination);
}
void prepareFlightPose(RE::PlayerCharacter* player,bool enabled){
    std::size_t i=0;
    if(enabled&&player&&loaderInstalled){
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(player->GetAnimationGraphManager(manager)&&manager){
            RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);
            for(auto& graph:manager->graphs)if(graph&&graph->holder==player&&i<characters.size()){
                const auto name=legs::canonical(graph->projectName.c_str()?graph->projectName.c_str():"");
                auto& character=graph->characterInstance;
                auto data=character.setup&&character.setup->data?character.setup->data->stringData.get():nullptr;
                if(pose::graph(name)&&slotIndex(data)!=0xffff)characters[i++]=&character;
            }
        }
    }
    while(i<characters.size())characters[i++]=nullptr;
}
void beforeFlightPoseActivate(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    if(!p||!p->animationName.c_str()||outgoingCopy(p,c))return;
    const bool wanted=eligible(c);
    auto nativeMode=pose::nativeMode(legs::canonical(p->animationName.c_str()));
    {
        std::lock_guard guard(lock);
        if(auto it=saved.find(p);it!=saved.end()&&same(it->second,p,c))nativeMode=static_cast<int>(it->second.modeIndex);
    }
    if(nativeMode<0)return;
    const auto mode=static_cast<unsigned>(nativeMode);
    const bool correctBinding=wanted&&currentPoseBinding(p,c,mode);std::lock_guard guard(lock);
    if(auto it=saved.find(p);it!=saved.end()){
        if(!same(it->second,p,c))saved.erase(it);
        else if(!wanted){restore(p,it->second);saved.erase(it);return;}
        else{
            const auto target=slotIndex(strings(c),mode);if(target==0xffff)return;
            p->animationName=pose::slots[mode].data();
            if(!correctBinding)p->animationBindingIndex=target;
            it->second.pose=target;it->second.modeIndex=mode;
            return;
        }
    }
    auto path=legs::canonical(p->animationName.c_str());
    if(!wanted)return;
    auto data=strings(c);const auto target=slotIndex(data,mode);if(target==0xffff)return;
    if(saved.size()>=256){SKSE::log::error("Single-file flight pose: backup capacity reached; clip={} entries={}",p->name.c_str(),saved.size());return;}
    auto native=p->animationBindingIndex;
    for(std::uint32_t i=0;i<static_cast<unsigned>(data->animationNames.size())&&i<0xffff;++i)
        if(data->animationNames[i].c_str()&&legs::canonical(data->animationNames[i].c_str())==path){native=static_cast<std::uint16_t>(i);break;}
    saved.emplace(p,Saved{c.character,RE::hkRefPtr<RE::hkbCharacterStringData>{data},p->name.c_str()?p->name.c_str():"",path,p->animationName,native,target,mode,
        p->cropStartAmountLocalTime,p->cropEndAmountLocalTime,p->startTime,p->playbackSpeed,p->enforcedDuration,*p->mode,p->triggers,p->originalTriggers});
    p->animationName=pose::slots[mode].data();p->animationBindingIndex=target;
    p->cropStartAmountLocalTime=p->cropEndAmountLocalTime=p->startTime=p->enforcedDuration=0;p->playbackSpeed=1;p->mode=RE::hkbClipGenerator::kModeLooping;
    p->triggers=nullptr;p->originalTriggers=nullptr;
    if(samples.fetch_add(1)<24)SKSE::log::info("Native flight pose entry: {} -> {} (native graph transitions; OAR conditional replacements)",path,pose::slots[mode]);
}
void afterFlightPoseActivate(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    if(!p||outgoingCopy(p,c))return;
    std::lock_guard guard(lock);
    if(auto it=saved.find(p);it!=saved.end()&&same(it->second,p,c)){
        // Locomotion's timed landing/sprint triggers cannot end an owned flight loop.
        // Combat generators never enter this route.
        p->triggers=nullptr;p->originalTriggers=nullptr;
        if(activatedPoseSamples.fetch_add(1)<32)
            SKSE::log::info("Flight pose activated: clip={} mode={} index={} path={} graphOwned={} marker={:X} binding={:X}",
                p->name.c_str(),pose::modes[it->second.modeIndex],p->animationBindingIndex,playing(p,c),graphOwnsClip(p,c),p->userData,reinterpret_cast<std::uintptr_t>(p->binding));
    }
}
void afterFlightPoseDeactivate(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    if(!p||outgoingCopy(p,c))return;
    const bool keep=eligible(c);std::lock_guard guard(lock);
    if(auto it=saved.find(p);it!=saved.end()){
        if(!same(it->second,p,c))saved.erase(it);
        else if(!keep){restore(p,it->second);saved.erase(it);}
    }
}
void updateFlightPose(RE::hkbClipGenerator* p,const RE::hkbContext& c){
    if(!p||!p->animationName.c_str()||outgoingCopy(p,c))return;
    const bool want=eligible(c);bool aliased{},motion{};
    {std::lock_guard guard(lock);if(auto it=saved.find(p);it!=saved.end()&&same(it->second,p,c)){aliased=true;motion=true;}else motion=pose::nativeMode(legs::canonical(p->animationName.c_str()))>=0;}
    if(!motion||(!aliased&&!selected(c))||!p->animationControl)return;
    // Only attach/detach the flight binding when ownership changes (takeoff,
    // drawing a weapon, landing). Never restart clips for a body-pose change:
    // idle/run/sprint nodes and OAR own both sides of their transitions.
    if(aliased!=want){
        if(bindingRepairSamples.fetch_add(1)<32)
            SKSE::log::info("Flight pose ownership changed: clip={} wanted={} index={} graphOwned={} marker={:X}",p->name.c_str(),want,p->animationBindingIndex,graphOwnsClip(p,c),p->userData);
        p->Deactivate(c);p->Activate(c);
    }
}
}
