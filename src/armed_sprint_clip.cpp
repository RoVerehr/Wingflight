#include "armed_sprint_clip.hpp"
#include "actor_state.hpp"
#include "logging.hpp"
#include "armed_sprint_clip_policy.hpp"
#include "lower_body_policy.hpp"
#include <SKSE/SKSE.h>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
namespace wingflight {
namespace {
std::array<std::atomic<const RE::hkbCharacter*>,8> characters{};
ArmedIdleGate gate{};
std::atomic_uint samples{};
std::atomic_uint activationSamples{},missingSamples{},repairSamples{};
struct SavedClip {
    const RE::hkbCharacter* character{}; // identity only
    RE::hkRefPtr<RE::hkbCharacterStringData> strings;
    std::string path;
    std::string nodeName;
    RE::hkStringPtr originalAnimationName;
    std::uint16_t nativeIndex{},idleIndex{};
    float cropStart{},cropEnd{},start{},speed{},duration{};
    RE::hkbClipGenerator::PlaybackMode mode{};
};
std::mutex clipLock;
std::unordered_map<RE::hkbClipGenerator*,SavedClip> saved;
struct IndexCache {
    RE::hkRefPtr<RE::hkbCharacterStringData> strings;
    std::unordered_map<std::string,std::uint16_t> indices;
};
std::mutex indexLock;
std::unordered_map<RE::hkbCharacterStringData*,IndexCache> indices;
std::unordered_set<std::string> loggedProjects;
RE::hkbCharacterStringData* stringData(const RE::hkbCharacter* character){
    return character&&character->setup&&character->setup->data
        ?character->setup->data->stringData.get():nullptr;
}
RE::hkbCharacterStringData* stringData(const RE::hkbContext& c){return stringData(c.character);}
std::uint16_t indexOf(RE::hkbCharacterStringData* data,std::string_view path){
    if(!data)return 0xffff;
    std::lock_guard lock(indexLock);
    auto& cache=indices[data];cache.strings=RE::hkRefPtr<RE::hkbCharacterStringData>{data};
    const std::string key{path};
    if(const auto it=cache.indices.find(key);it!=cache.indices.end())return it->second;
    const auto& names=data->animationNames;
    const auto index=resolveNativeIdleSlot(static_cast<std::uint32_t>(names.size()),
        [&](std::uint32_t i){return names[i].c_str()?legs::canonical(names[i].c_str()):std::string{};},path);
    cache.indices.emplace(key,index);return index;
}
bool selected(const RE::hkbContext& c){
    if(!c.character)return false;
    for(auto& character:characters)if(character.load()==c.character)return true;
    return false;
}
bool wanted(RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    return clip&&routeSprintIdle(selected(c),gate&&gate(RE::PlayerCharacter::GetSingleton()),true,clip->userData==0xC);
}
bool same(const SavedClip& s,RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    // The graph node's name is stable across binding replacement; animationName
    // may be normalized/reassigned while loading a new binding.
    return s.character==c.character&&s.strings.get()==stringData(c)
        &&clip->name.c_str()&&s.nodeName==clip->name.c_str();
}
std::string bindingPath(RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    const auto data=stringData(c);const auto index=clip->animationBindingIndex;
    return data&&index<data->animationNames.size()&&data->animationNames[index].c_str()
        ?legs::canonical(data->animationNames[index].c_str()):std::string{};
}
void restore(RE::hkbClipGenerator* clip,const SavedClip& s){
    clip->animationBindingIndex=s.nativeIndex;
    clip->animationName=s.originalAnimationName;
    clip->cropStartAmountLocalTime=s.cropStart;clip->cropEndAmountLocalTime=s.cropEnd;
    clip->startTime=s.start;clip->playbackSpeed=s.speed;clip->enforcedDuration=s.duration;clip->mode=s.mode;
}
}
void initializeArmedSprintIdle(ArmedIdleGate eligibility){gate=eligibility;}
void prepareArmedSprintIdle(RE::PlayerCharacter* player,bool fullFlight){
    std::size_t i=0;
    if(fullFlight&&player){
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(player->GetAnimationGraphManager(manager)&&manager){
            RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
            for(const auto& graph:manager->graphs)if(graph&&graph->holder==player&&i<characters.size()){
                const auto character=&graph->characterInstance;
                const auto project=legs::canonical(graph->projectName.c_str()?graph->projectName.c_str():"");
                const auto data=stringData(character);
                const bool hasBodyIdle=indexOf(data,"animations/male/mt_idle.hkx")!=0xffff;
                const bool body=thirdPersonIdleProject(project,hasBodyIdle,false,false);
                if(body)characters[i++]=character;
                {std::lock_guard indexGuard(indexLock);
                    if(logging::detailed()&&loggedProjects.size()<16&&loggedProjects.insert(project).second)
                        SKSE::log::debug("Armed sprint idle project: project={} bodyIdle={} selected={}",project,hasBodyIdle,body);}
            }
        }
    }
    while(i<characters.size())characters[i++]=nullptr;
    // Keep dormant backups until their own Activate/Deactivate restores them.
    // Clearing on C/load could leave an inactive, reused graph aliased on ground.
}
void beforeArmedSprintActivate(RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    if(!clip||clip->userData==0xC||!clip->animationName.c_str())return;
    auto path=legs::canonical(clip->animationName.c_str());
    {std::lock_guard lock(clipLock);const auto it=saved.find(clip);
        if(it!=saved.end()&&same(it->second,clip,c))path=it->second.path;}
    const auto idle=sprintIdleSlot(path);if(idle.empty())return;
    const bool shouldAlias=wanted(clip,c);
    std::lock_guard lock(clipLock);
    if(const auto it=saved.find(clip);it!=saved.end()){
        if(!same(it->second,clip,c)){saved.erase(it);}
        else if(!shouldAlias){restore(clip,it->second);saved.erase(it);return;}
        else{
            // OAR can deactivate/activate internally while changing an idle variant.
            // Its new replacement index must pass through unchanged. A true graph
            // deactivation has already restored the idle original index in OAR.
            // A dormant native graph or an inherited ground OAR cache can restore
            // the sprint index. Keep actual idle variants; remap ground bindings.
            if(!idleSlotBinding(bindingPath(clip,c),idle))clip->animationBindingIndex=it->second.idleIndex;
            return;
        }
    }
    if(!shouldAlias)return;
    auto data=stringData(c);const auto from=indexOf(data,path),to=indexOf(data,idle);
    if(from==0xffff||to==0xffff){
        if(missingSamples.fetch_add(1)<16)SKSE::log::warn("Armed sprint idle slot missing: {} -> {} nativeIndex={} idleIndex={}",path,idle,from,to);
        return;
    }
    SavedClip s{c.character,RE::hkRefPtr<RE::hkbCharacterStringData>{data},path,clip->name.c_str()?clip->name.c_str():"",clip->animationName,from,to,clip->cropStartAmountLocalTime,clip->cropEndAmountLocalTime,
        clip->startTime,clip->playbackSpeed,clip->enforcedDuration,*clip->mode};
    saved.emplace(clip,s);
    clip->animationBindingIndex=to;
    clip->cropStartAmountLocalTime=clip->cropEndAmountLocalTime=clip->startTime=clip->enforcedDuration=0;
    clip->playbackSpeed=1;clip->mode=RE::hkbClipGenerator::kModeLooping;
    // Our Activate wrapper runs BEFORE the existing OAR wrapper. Its ActiveClip
    // records the idle index, then chooses the user's idle replacement normally.
    if(logging::detailed()&&samples.fetch_add(1)<32)SKSE::log::debug("Armed sprint idle slot: {} -> {} (native sprint branch retained)",path,idle);
}
void afterArmedSprintActivate(RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    if(!clip||clip->userData==0xC)return;
    std::uint16_t idleIndex{};
    {std::lock_guard lock(clipLock);const auto it=saved.find(clip);
        if(it==saved.end()||!same(it->second,clip,c))return;idleIndex=it->second.idleIndex;}
    if(!logging::detailed()||activationSamples.fetch_add(1)>=32)return;
    const auto data=stringData(c);const auto playing=clip->animationBindingIndex;
    const auto name=data&&playing<data->animationNames.size()?data->animationNames[playing].c_str():nullptr;
    SKSE::log::debug("Armed sprint idle activation: idleIndex={} playingIndex={} playing={}",idleIndex,playing,name?name:"<unavailable>");
}
void afterArmedSprintDeactivate(RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    if(!clip||clip->userData==0xC)return;
    const bool keep=wanted(clip,c);
    std::lock_guard lock(clipLock);
    const auto it=saved.find(clip);if(it==saved.end())return;
    if(!same(it->second,clip,c)){saved.erase(it);return;}
    // OAR owns its replacement/variant indices. Restore only after its native
    // Deactivate and ActiveClip cleanup, and only when flight-idle ownership ends.
    if(!keep){restore(clip,it->second);saved.erase(it);}
}
void updateArmedSprintIdle(RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    if(!clip||clip->userData==0xC||!clip->animationName.c_str())return;
    auto path=legs::canonical(clip->animationName.c_str());
    bool alias=false;
    {std::lock_guard lock(clipLock);const auto it=saved.find(clip);alias=it!=saved.end()&&same(it->second,clip,c);
        if(alias)path=it->second.path;}
    const auto idle=sprintIdleSlot(path);if(idle.empty())return;
    if(!alias&&!selected(c))return;
    const auto player=RE::PlayerCharacter::GetSingleton();
    if(!player||!player->Is3DLoaded()||player->IsDead()||knockedDown(player)||player->IsInKillMove()
        ||player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth)<=0)return;
    const bool desired=wanted(clip,c)&&(alias||indexOf(stringData(c),idle)!=0xffff);
    const auto playing=desired&&alias?bindingPath(clip,c):std::string{};
    const bool repair=repairSprintBinding(desired,alias,idleSlotBinding(playing,idle),clip->animationControl!=nullptr,false);
    if(!repair&&!restartSprintClip(alias,desired,clip->animationControl!=nullptr,false))return;
    if(logging::detailed()&&repair&&repairSamples.fetch_add(1)<16)SKSE::log::debug("Armed sprint idle: rebinding inherited ground/OAR selection {} to {}",playing,idle);
    // Weapon drawing/sheathing or C can change ownership while this sprint clip
    // remains active. Rebind only that clip; keep the state machine and all input
    // events. OAR uses the same Deactivate/Activate lifetime when replacing clips.
    clip->Deactivate(c);clip->Activate(c);
}
}
