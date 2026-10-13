#include "flight_audio.hpp"
#include "diagnostics.hpp"
#include <SKSE/SKSE.h>
#include <unordered_set>
#include <vector>

namespace wingflight {
namespace {
constexpr char breathVariable[]="Wingflight_MuteSprintBreath";
RE::BSFixedString breathConditionName;
std::unordered_set<std::string> footTags;
std::unordered_set<const RE::BGSSoundDescriptorForm*> footSounds;
std::vector<RE::TESTopicInfo*> breathInfos;
bool installed{},enabled{};
RE::BGSSoundDescriptorForm* collisionSound{};
RE::TESTopic* collisionPainTopic{};
class PainCompleted final : public RE::BSScript::IStackCallbackFunctor {
public:
    void operator()(RE::BSScript::Variable) override {SKSE::log::info("Flight collision pain: native Say call completed (audible voice depends on topic/voice availability)");}
    void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
};
void playCollisionPain(RE::PlayerCharacter* player){
    if(!player||player->IsDead())return;
    auto vm=RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if(!vm||!collisionPainTopic){diagnostic::report("collision.pain-topic-unavailable","retain impact sound without pain voice",player);return;}
    const auto handle=vm->GetObjectHandlePolicy()->GetHandleForObject(static_cast<RE::VMTypeID>(player->GetFormType()),player);
    RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new PainCompleted());
    const bool queued=vm->DispatchMethodCall(handle,"Actor","Say",
        RE::MakeFunctionArguments(static_cast<RE::TESTopic*>(collisionPainTopic),static_cast<RE::Actor*>(nullptr),false),callback);
    SKSE::log::info("Flight collision pain: topic={:08X} nativeSayQueued={} (current player voice; no extra damage)",collisionPainTopic->GetFormID(),queued);
    if(!queued)diagnostic::report("collision.pain-dispatch-failed","retain impact sound without pain voice",player);
}

void installBreathConditions(RE::PlayerCharacter* player){
    if(installed||!player)return;
    // Prefix one complete OR group, then preserve the original AND/OR chain:
    // (speaker != player OR Wingflight_MuteSprintBreath == 0) AND originals.
    // Native dialogue selection skips the line cleanly before voice allocation.
    // These are ordinary in-memory conditions, never written to a plugin/save.
    for(auto* info:breathInfos){
        auto* npc=new RE::TESConditionItem();
        auto* inactive=new RE::TESConditionItem();
        npc->data.functionData.function=RE::FUNCTION_DATA::FunctionID::kGetIsReference;
        npc->data.functionData.params[0]=player;
        npc->data.comparisonValue.f=0;
        npc->data.flags.isOR=true;
        inactive->data.functionData.function=RE::FUNCTION_DATA::FunctionID::kGetGraphVariableInt;
        // The native condition forwards parameter 1 as const BSFixedString&
        // to IAnimationGraphManagerHolder::GetGraphVariableInt, not char*.
        inactive->data.functionData.params[0]=&breathConditionName;
        inactive->data.comparisonValue.f=0;
        inactive->next=info->objConditions.head;
        npc->next=inactive;
        info->objConditions.head=npc;
    }
    installed=true;
    SKSE::log::info("Flight sprint-breath condition guard installed: {} dialogue infos; NPCs and grounded player unchanged",breathInfos.size());
}
}
void initializeFlightAudio(bool muteBreath){
    enabled=muteBreath;
    const auto handler=RE::TESDataHandler::GetSingleton();
    if(!handler)return;
    // Vanilla unarmed impact against flesh; sex/race independent and compatible
    // with sound replacers. No copied sound asset or generated hit event.
    collisionSound=handler->LookupForm<RE::BGSSoundDescriptorForm>(0xC190D,"Skyrim.esm");
    collisionPainTopic=handler->LookupForm<RE::TESTopic>(0x13EBB,"Skyrim.esm");
    for(auto* foot:handler->GetFormArray<RE::BGSFootstep>()){
        if(!foot||!foot->tag.c_str()||!std::string_view(foot->tag.c_str()).starts_with("Foot"))continue;
        footTags.emplace(foot->tag.c_str());
        if(!foot->impactSet)continue;
        for(const auto& [material,impact]:foot->impactSet->impactMap){
            if(!impact)continue;
            if(impact->sound1)footSounds.insert(impact->sound1);
            if(impact->sound2)footSounds.insert(impact->sound2);
        }
    }
    if(!enabled)return;
    breathConditionName=breathVariable;
    // Read subtype values from the effective records rather than assuming enum
    // numbers are identical between 1.6.1170 and the new 1.7.x dialogue types.
    auto* sprint=handler->LookupForm<RE::TESTopic>(0x1701C,"Skyrim.esm");
    auto* exhausted=handler->LookupForm<RE::TESTopic>(0x5DD77,"Skyrim.esm");
    if(!sprint||!exhausted){enabled=false;SKSE::log::warn("Sprint breath records unavailable; voice guard disabled");return;}
    std::unordered_set<RE::TESTopicInfo*> seen;
    for(auto* topic:handler->GetFormArray<RE::TESTopic>()){
        if(!topic||!((topic->data.type==sprint->data.type&&topic->data.subtype==sprint->data.subtype)
            ||(topic->data.type==exhausted->data.type&&topic->data.subtype==exhausted->data.subtype)))continue;
        for(std::uint32_t i=0;i<topic->numTopicInfos;++i){
            auto* info=topic->topicInfos[i];
            if(info&&seen.insert(info).second)breathInfos.push_back(info);
        }
    }
    SKSE::log::info("Flight audio registry: {} foot tags, {} foot sounds, {} sprint/exhaustion infos",footTags.size(),footSounds.size(),breathInfos.size());
}
void updateFlightBreath(RE::PlayerCharacter* player,bool fullAirborneFlight){
    if(!enabled||!player)return;
    // Wait for a real BDI-injected player graph before attaching any conditions.
    // The declared default is zero; no voice changes if injection is unavailable.
    if(player->SetGraphVariableInt(breathConditionName,fullAirborneFlight?1:0))installBreathConditions(player);
}
bool registeredFootContact(std::string_view tag){return footTags.contains(std::string(tag));}
void playFlightCollisionSound(RE::PlayerCharacter* player){
    playCollisionPain(player);
    const auto audio=RE::BSAudioManager::GetSingleton();
    if(!player||!collisionSound||!audio){
        diagnostic::report("collision.sound-unavailable","skip impact sound; collision release continues",player);return;
    }
    RE::BSSoundHandle handle;
    const bool built=audio->GetSoundHandle(handle,collisionSound);bool played=false;
    if(built){handle.SetPosition(player->GetPosition());if(auto node=player->Get3D())handle.SetObjectToFollow(node);played=handle.Play();}
    SKSE::log::info("Flight collision sound: descriptor={:08X} built={} played={}",collisionSound->GetFormID(),built,played);
    if(!built||!played)diagnostic::report("collision.sound-play-failed","skip impact sound; collision release continues",player);
}
bool registeredFootSound(std::string_view name){
    if(name.empty())return false;
    const auto form=RE::TESForm::LookupByEditorID(name);
    const auto sound=form?form->As<RE::BGSSoundDescriptorForm>():nullptr;
    return sound&&footSounds.contains(sound);
}
}
