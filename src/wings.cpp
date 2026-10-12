#include "wings.hpp"
#include "effect_owner.hpp"
#include "awu_compatibility.hpp"
#include "vampire_runtime.hpp"
#include "continuous_wing_rate.hpp"
#include <SKSE/SKSE.h>
#include <unordered_set>

namespace wingflight {
namespace {
struct ContinuousWingBinding {
    std::array<RE::BSTSmartPointer<RE::BShkbAnimationGraph>,16> graphs;
    std::array<RE::NiPointer<RE::NiAVObject>,16> roots;
    unsigned count{};
    std::atomic<float> rate{};
};
std::atomic<std::shared_ptr<ContinuousWingBinding>> continuousBinding;
std::atomic_uint continuousRateSamples{};
std::unordered_map<const RE::ModelReferenceEffect*,ContinuousWingCycleRequest> continuousCycles;
void clearContinuousBinding(){
    if(auto old=continuousBinding.exchange(nullptr))old->rate=0;
    continuousCycles.clear();
}
}
void Wings::prepareContinuousAnimation(WingMatch match,bool active,bool highSpeed,float fastMultiplier,float dt){
    if(!active||!match.profile||!match.profile->continuousFlight||!wingEventActor(match.owner,match.effect)
        ||!match.effect->hitEffects){clearContinuousBinding();return;}
    std::array<RE::BSTSmartPointer<RE::BShkbAnimationGraph>,16> graphs;
    std::array<RE::NiPointer<RE::NiAVObject>,16> roots;
    unsigned count{};
    std::unordered_set<const RE::ModelReferenceEffect*> currentModels;
    const auto stateTable=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    for(auto effect:*match.effect->hitEffects) {
        if(!effect||effect->finished||effect->GetType()!=RE::TEMP_EFFECT_TYPE::kRefModel)continue;
        const auto model=static_cast<RE::ModelReferenceEffect*>(effect);
        if(!model->GetAttached()||!model->Get3D())continue;
        currentModels.insert(model);
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(!model->GetAnimationGraphManager(manager)||!manager){
            diagnostic::report("continuous.wing-graph-missing","retain provider animation rate",model,match.profile->id);continue;}
        int wingState=-1;
        {
            RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
            for(const auto& graph:manager->graphs)if(graph) {
                if(count==graphs.size()){diagnostic::report("continuous.graph-capacity","skip excess wing graphs",model,match.profile->id,count,static_cast<double>(graphs.size()));break;}
                graphs[count]=graph;roots[count].reset(model->Get3D());++count;
                const auto behavior=graph->characterInstance.behaviorGraph;
                if(!behavior||!behavior->activeNodes||behavior->activeNodes->size()>128)continue;
                for(const auto& node:*behavior->activeNodes) {
                    auto clone=node.nodeClone;
                    if(!clone||*reinterpret_cast<std::uintptr_t*>(clone)!=stateTable)continue;
                    const auto state=static_cast<const RE::hkbStateMachine*>(clone);
                    if(state->isActive&&state->name.c_str()&&std::string_view(state->name.c_str())=="StateMachine00"){
                        wingState=state->currentStateID;break;}
                }
            }
        }
        if(wingState<0)diagnostic::report("continuous.wing-state-unavailable","retain frequency override and retry original wing state",model,match.profile->id);
        if(continuousCycles[model].update(dt,active,wingState)) {
            // Clip02 crops the original loop to a narrow tremor. Select the
            // original full-loop jump state on the ART after releasing its lock.
            const bool accepted=model->NotifyAnimationGraph("JumpStandingStart");
            SKSE::log::info("Continuous wing full-cycle request: adapter={} previousState={} requestedState=2 event=JumpStandingStart accepted={} (wing ART only)",match.profile->id,wingState,accepted);
            if(!accepted)diagnostic::report("continuous.full-cycle-request-rejected","retain rate override and retry",model,match.profile->id);
        }
    }
    if(!count){clearContinuousBinding();return;}
    std::erase_if(continuousCycles,[&](const auto& item){return !currentModels.contains(item.first);});
    const float rate=continuousWingRate(active,highSpeed,fastMultiplier);
    auto current=continuousBinding.load();
    bool same=current&&current->count==count;
    if(same)for(unsigned i=0;i<count;++i)if(current->graphs[i].get()!=graphs[i].get()||current->roots[i].get()!=roots[i].get()){same=false;break;}
    if(same){
        const float previous=current->rate.exchange(rate);
        if(previous!=rate)SKSE::log::info("Continuous wing frequency changed: adapter={} playbackSpeed={:.2f} highSpeed={} FastFlapPlaybackMultiplier={:.2f}",match.profile->id,rate,highSpeed,fastMultiplier);
        return;
    }
    auto next=std::make_shared<ContinuousWingBinding>();next->graphs=std::move(graphs);next->roots=std::move(roots);next->count=count;next->rate=rate;
    if(auto old=continuousBinding.exchange(next))old->rate=0;
    SKSE::log::info("Continuous wing animation registered: adapter={} ownedGraphs={} playbackSpeed={:.2f}; jump rate in flight including Descend, sprint uses FastFlapPlaybackMultiplier",match.profile->id,count,rate);
}
float continuousWingClipRate(const RE::hkbClipGenerator* clip,const RE::hkbContext& context){
    if(!clip||!context.character)return 0;
    const auto state=continuousBinding.load();if(!state)return 0;
    const float rate=state->rate.load();if(rate<=0)return 0;
    bool owned=false;for(unsigned i=0;i<state->count;++i)if(&state->graphs[i]->characterInstance==context.character){owned=true;break;}
    if(!owned)return 0;
    if(!clip->name.c_str()||!continuousWingClip(clip->name.c_str(),clip->mode==RE::hkbClipGenerator::kModeLooping)){
        diagnostic::report("continuous.clip-layout-unrecognized","retain provider animation rate",clip,clip->name.c_str()?clip->name.c_str():"<unnamed>");return 0;}
    return rate;
}
void observeContinuousWingClip(const RE::hkbClipGenerator* clip,float before,float dt,float rate){
    if(clip&&rate>0&&continuousRateSamples.fetch_add(1)<12)
        SKSE::log::info("Continuous wing rate applied: clip={} flightRate={:.2f} restoredProviderRate={:.2f} timeBefore={:.4f} timeAfter={:.4f} nativeStep={:.4f} (wing graph only)",
            clip->name.c_str(),rate,clip->playbackSpeed,before,clip->localTime,dt);
}
bool Wings::initialize(float amplitude){artReady_=runtime_.install(amplitude);return artReady_;}
void Wings::advanceVisual(float delta){runtime_.advance(delta);advanceVampireWings(delta);if(logicalActive_)logical_.advance(delta);}
void Wings::clearVisual(bool smooth){clearContinuousBinding();strokeSound_.reset();fastSound_.reset();flutterSound_.reset();runtime_.release(smooth);releaseVampireWings(smooth);nativeActive_=logicalActive_=false;logical_.reset();}
WingPlayback Wings::prepareVisual(WingMatch match){
    if(match.profile&&match.profile->animationless) {
        WingPlayback out;if(!wingEventActor(match.owner,match.effect))return out;
        if(!logicalActive_) {
            runtime_.release(false);releaseVampireWings(false);nativeActive_=false;
            logical_.reset(match.profile->timing.glideEntryPhase);logicalActive_=true;
        }
        out.found=out.active=out.gliding=out.spreadActive=true;
        out.phase=logical_.phase;out.frequency=logical_.rate;out.duration=LogicalWings::duration;
        out.glidePhase=logical_.fastPhase;out.glideFrequency=logical_.fastRate;
        out.spreadPhase=logical_.spreadPhase;out.spreadFrequency=logical_.spreadRate;
        out.flapWeight=out.graphWeight=logical_.weights.flap;
        out.glideWeight=out.graphGlideWeight=logical_.weights.fast;
        out.spreadWeight=out.graphSpreadWeight=logical_.weights.spread;out.otherWeight=logical_.weights.original;
        out.activeSequence="Logical dragon wingbeats (no model)";return out;
    }
    if(logicalActive_){logicalActive_=false;logical_.reset();strokeSound_.reset();fastSound_.reset();flutterSound_.reset();}
    if(match.profile&&match.profile->continuousFlight) {
        // Fairy/Chaurus own their looping Havok animation. No clock, pose or
        // generated Ni interpolator is borrowed from that provider.
        WingPlayback out;
        if(!wingEventActor(match.owner,match.effect)||!match.effect->hitEffects)return out;
        for(auto effect:*match.effect->hitEffects)if(effect&&!effect->finished&&effect->GetType()==RE::TEMP_EFFECT_TYPE::kRefModel) {
            const auto model=static_cast<RE::ModelReferenceEffect*>(effect);
            if(model->GetAttached()&&model->Get3D()){out.found=true;out.activeSequence="Provider-owned continuous loop";break;}
        }
        return out;
    }
    if(match.profile&&nativeActive_!=match.profile->nativeVampire){
        runtime_.release(false);releaseVampireWings(false);
        strokeSound_.reset();fastSound_.reset();flutterSound_.reset();
    }
    if(match.profile&&match.profile->nativeVampire){nativeActive_=true;return prepareVampireWings(match.owner);}
    nativeActive_=false;if(!artReady_)return {};
    if(!match.profile||!wingEventActor(match.owner,match.effect))return {};
    return runtime_.prepare(match);
}
bool Wings::setBlend(WingMatch match,float flap,float ownership,float fast)const{
    if(match.profile&&match.profile->animationless) {
        if(!wingEventActor(match.owner,match.effect))return false;
        const_cast<Wings*>(this)->logical_.blend(flap,ownership,fast);return true;
    }
    if(match.profile&&match.profile->continuousFlight)return wingEventActor(match.owner,match.effect)!=nullptr;
    if(match.profile&&match.profile->nativeVampire)return blendVampireWings(flap,ownership,fast);
    if(!match.profile||!wingEventActor(match.owner,match.effect))return false;
    return const_cast<WingRuntime&>(runtime_).blend(flap,ownership,fast);
}
bool Wings::parkInvisibleFlap(){return logicalActive_?logical_.parkPower():nativeActive_?parkVampirePower():runtime_.parkPower();}
bool Wings::parkInvisibleGlide(){return logicalActive_?logical_.parkFast():nativeActive_?parkVampireFast():runtime_.parkFast();}
void Wings::setFlapRate(float rate){if(logicalActive_)logical_.powerRate(rate);else if(nativeActive_)rateVampireWings(rate);else runtime_.powerRate(rate);}
void Wings::updateGlideRate(float fast,float spread){if(logicalActive_)logical_.loopRate(fast,spread);else if(nativeActive_)loopRateVampireWings(fast,spread);else runtime_.loopRate(fast,spread);}
void Wings::updatePowerPitch(float degrees){if(logicalActive_)return;if(nativeActive_)pitchVampireWings(degrees);else runtime_.pitch(degrees);}
void Wings::resolve(const Configuration& cfg) {
    effects_.clear();
    sounds_.clear();
    fastSounds_.clear();
    spreadSounds_.clear();
    const auto data = RE::TESDataHandler::GetSingleton();
    resolveVampireFlight(cfg);
    vampireProfile_={};vampireProfile_.id="Vanilla Vampire Lord";vampireProfile_.plugin="Dawnguard.esm";
    vampireProfile_.nativeVampire=true;vampireProfile_.flapSoundID=0x19d7f;
    const auto nativeIni=std::filesystem::absolute("Data/SKSE/Plugins/Wingflight/VampireLord.ini").string();
    auto timingValue=[&](const char* key,float fallback,float minimum,float maximum){
        char text[64]{};GetPrivateProfileStringA("Timing",key,"",text,sizeof(text),nativeIni.c_str());
        if(!text[0])return fallback;char* end{};const float value=std::strtof(text,&end);
        if(end==text||*end||!std::isfinite(value)||value<minimum||value>maximum){
            SKSE::log::warn("Native Vampire Lord timing rejected: key={} value='{}' allowed={}..{}; default={}",key,text,minimum,maximum,fallback);return fallback;}
        return value;};
    auto& nativeTiming=vampireProfile_.timing;
    nativeTiming.expandSeconds=timingValue("ExpandSeconds",.32f,.05f,1);
    nativeTiming.upstrokeSeconds=timingValue("UpstrokeSeconds",.40f,.10f,2);
    nativeTiming.downstrokeSeconds=timingValue("DownstrokeSeconds",.34f,.08f,2);
    nativeTiming.fastUpstrokeSeconds=timingValue("FastUpstrokeSeconds",.20f,.10f,nativeTiming.upstrokeSeconds);
    nativeTiming.fastDownstrokeSeconds=timingValue("FastDownstrokeSeconds",.34f,.08f,2);
    nativeTiming.normalFlapSeconds=timingValue("NormalFlapSeconds",1,.2f,5);
    nativeTiming.toFlapSeconds=timingValue("ToFlapSeconds",.32f,.10f,2);
    nativeTiming.toGlideSeconds=timingValue("ToRestSeconds",.40f,.10f,2);
    vampireProfile_.flapSoundPhase=0.08f;vampireProfile_.fastSoundID=0;vampireProfile_.spreadSoundID=0;
    if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(0x19d7f,"Dawnguard.esm")){
        sounds_.emplace(&vampireProfile_,sound);}
    else SKSE::log::warn("Native Vampire Lord wing sound Dawnguard.esm:019D7F unavailable");
    for (const auto& profile : cfg.profiles) {
        if(!data->LookupLoadedModByName(profile.plugin) && !data->LookupLoadedLightModByName(profile.plugin)){
            SKSE::log::info("Optional wing adapter {} skipped: {} is not installed",profile.id,profile.plugin);continue;
        }
        if(profile.flapSoundID) {
            if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(profile.flapSoundID,profile.plugin)) {
                sounds_.emplace(&profile,sound);
            } else { SKSE::log::warn("Missing power-stroke sound {}:{:06X}",profile.plugin,profile.flapSoundID); }
        }
        if(profile.fastSoundID) {
            if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(profile.fastSoundID,profile.plugin))fastSounds_.emplace(&profile,sound);
            else SKSE::log::warn("Missing fast-flap sound {}:{:06X}",profile.plugin,profile.fastSoundID);
        }
        if(profile.spreadSoundID&&profile.spreadSoundVolume>0) {
            if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(profile.spreadSoundID,profile.plugin))spreadSounds_.emplace(&profile,sound);
            else SKSE::log::warn("Missing flutter sound {}:{:06X}",profile.plugin,profile.spreadSoundID);
        }
        unsigned count = 0;
        for (auto id : profile.effectIDs) {
            if (auto effect = data->LookupForm<RE::EffectSetting>(id, profile.plugin)) {
                if (!effects_.emplace(effect, &profile).second) {
                    SKSE::log::warn("Duplicate wing effect {:08X}; first adapter wins", effect->GetFormID());
                } else { ++count; }
            } else { SKSE::log::warn("Missing adapter effect {}:{:06X}", profile.plugin, id); }
        }
        SKSE::log::info("Wing adapter {} resolved {}/{} visible effects", profile.id, count, profile.effectIDs.size());
    }
}
void Wings::updatePowerSound(WingMatch match,const WingPlayback& playback){
    if(match.profile&&match.profile->continuousFlight){strokeSound_.reset();fastSound_.reset();flutterSound_.reset();return;}
    if(!match.profile||(match.profile->nativeVampire?!isNativeVampireLord(match.owner):!wingEventActor(match.owner,match.effect))){strokeSound_.reset();fastSound_.reset();flutterSound_.reset();return;}
    const auto& profile=*match.profile;
    const bool power=strokeSound_.update(playback.found&&playback.active,playback.phase,
        playback.flapWeight,playback.frequency,profile.flapSoundPhase,profile.timing.downstrokeEnd);
    const bool fast=fastSound_.update(playback.found&&playback.gliding,playback.glidePhase,
        playback.glideWeight,playback.glideFrequency,profile.fastSoundPhase,1.0f);
    const bool flutter=flutterSound_.update(playback.found&&playback.spreadActive,playback.spreadPhase,
        playback.spreadWeight,playback.spreadFrequency,profile.spreadSoundPhase1,profile.spreadSoundPhase2);
    auto play=[&](const auto& descriptors,const char* kind,float phase,float volume=1.0f){
        const auto found=descriptors.find(match.profile);auto audio=RE::BSAudioManager::GetSingleton();
        if(found==descriptors.end()||!audio){
            // Sound ID/volume zero is an intentional mute, not a missing record.
            const bool configured=std::string_view(kind)=="power-stroke"?profile.flapSoundID!=0
                :std::string_view(kind)=="fast-flap"?profile.fastSoundID!=0:profile.spreadSoundID!=0&&profile.spreadSoundVolume>0;
            if(configured)diagnostic::report(!audio?"sound.audio-manager-missing":"sound.descriptor-unresolved",
                "skip sound playback",match.owner,kind);
            return;}
        RE::BSSoundHandle handle;const bool built=audio->GetSoundHandle(handle,found->second);bool played=false;
        if(built){handle.SetPosition(match.owner->GetPosition());if(auto node=match.owner->Get3D())handle.SetObjectToFollow(node);
            if(volume!=1.0f)handle.SetVolume(volume);played=handle.Play();}
        SKSE::log::info("Runtime {} sound: phase={:.4f} built={} played={} descriptor={:08X}",kind,phase,built,played,found->second->GetFormID());
        if(!built||!played)diagnostic::report(!built?"sound.handle-build-failed":"sound.play-failed","skip this playback",match.owner,
            fmt::format("kind={} descriptor={:08X}",kind,found->second->GetFormID()));
    };
    if(power)play(sounds_,"power-stroke",playback.phase);
    if(fast)play(fastSounds_,"fast-flap",playback.glidePhase);
    if(flutter)play(spreadSounds_,"flutter-glide",playback.spreadPhase,
        profile.spreadSoundVolume*std::clamp(playback.spreadWeight,0.0f,1.0f));
}
WingMatch Wings::find(RE::Actor* actor,bool ownedFlight) const {
    if (!actor) { return {}; }
    // Body graph decides stance/events. A visible AWU ART is independent of
    // whether the player currently uses a VampireLord or Character body graph.
    if(usesVampireLordBehavior(actor)){
        bool stance{};
        if(!actor->GetGraphVariableBool("isLevitating",stance)||(!stance&&!ownedFlight))return {};
    }
    WingMatch art,invisible;
    const auto active = actor->AsMagicTarget()->GetActiveEffectList();
    if (active&&artReady_)for (auto effect : *active) {
        if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) { continue; }
        const auto found = effects_.find(effect->GetBaseObject());
        if (found != effects_.end()) {
            if(found->second->awuCompatibility && !awuEffectReady(actor,effect))continue;
            if(found->second->animationless) {
                if(wingEventActor(actor,effect))invisible={found->second,effect,actor};
                continue;
            }
            if(!wingEventActor(actor,effect)||!effect->hitEffects)continue;
            bool attached=false;
            for(auto visual:*effect->hitEffects)if(visual&&!visual->finished&&visual->GetType()==RE::TEMP_EFFECT_TYPE::kRefModel){
                const auto model=static_cast<RE::ModelReferenceEffect*>(visual);
                if(model->GetAttached()&&model->Get3D()){attached=true;break;}}
            if(attached){art={found->second,effect,actor};break;}
        }
    }
    const bool skeletal=nativeVampireFlightEligible(actor,ownedFlight)&&vampireSceneReady();
    const auto requested=isNativeVampireLord(actor)?vampireWingDriver():vampire::Driver::automatic;
    const auto selected=vampire::chooseDriver(requested,
        art.profile!=nullptr,skeletal);
    if(selected==vampire::Driver::effect)return art;
    if(selected==vampire::Driver::skeleton)return {&vampireProfile_,nullptr,actor};
    // VLrn beast wings may carry the invisible AWU helper simultaneously.
    // A real ART or visible skeletal rig must win over this model-less fallback.
    if(vampire::useInvisibleFallback(requested==vampire::Driver::automatic,art.profile!=nullptr,skeletal,invisible.profile!=nullptr))return invisible;
    return {};
}
bool Wings::send(WingMatch match,const std::string& event)const{
    if(match.profile&&match.profile->nativeVampire){
        if(event==match.profile->land||event==match.profile->fall)return true;
        return const_cast<Wings*>(this)->prepareVisual(match).found;
    }
    if(!match.profile||!wingEventActor(match.owner,match.effect)||event.empty())return false;
    // These are native-driver commands, never events requiring a patched HKX.
    // Original AWU listeners still receive the player's normal body/lifecycle events.
    if(event==match.profile->land||event==match.profile->fall)return true;
    return const_cast<Wings*>(this)->prepareVisual(match).found;
}
void Wings::updateGroundRecovery(WingMatch match,float dt,bool grounded,bool fullFlight){
    // Other wing providers keep their own behavior. Borrow only freshly validated
    // player-owned art models; no object from an old effect/session is dereferenced.
    if(!match.profile||!match.profile->awuCompatibility||match.profile->continuousFlight||!wingEventActor(match.owner,match.effect)
        ||!match.effect->hitEffects){groundRecovery_.clear();return;}
    std::unordered_set<const RE::ModelReferenceEffect*> current;
    const auto stateTable=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    for(auto effect:*match.effect->hitEffects){
        if(!effect||effect->finished||effect->GetType()!=RE::TEMP_EFFECT_TYPE::kRefModel)continue;
        auto* model=static_cast<RE::ModelReferenceEffect*>(effect);
        if(!model->GetAttached())continue;
        current.insert(model);
        bool loop=false;
        if(grounded&&!fullFlight){
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
            if(model->GetAnimationGraphManager(manager)&&manager){
                RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
                for(const auto& graph:manager->graphs){
                    if(!graph||graph->projectName!="AnimatedDragonWings")continue;
                    auto behavior=graph->characterInstance.behaviorGraph;
                    if(!behavior||!behavior->activeNodes||behavior->activeNodes->size()>128)continue;
                    // The rootGenerator is an inactive template; rootGeneratorClone
                    // is null in 1170. Inspect the real evaluated nodeClone instead.
                    for(const auto& info:*behavior->activeNodes){
                        auto* node=info.nodeClone;
                        if(!node||*reinterpret_cast<std::uintptr_t*>(node)!=stateTable)continue;
                        const auto* state=static_cast<const RE::hkbStateMachine*>(node);
                        if(!state->name.c_str()||std::string_view(state->name.c_str())!="HandCastingFXBehavior")continue;
                        loop=state->isActive&&state->currentStateID==8;
                        if(loop)break;
                    }
                    if(loop)break;
                }
            }
        }
        if(groundRecovery_[model].update(dt,grounded,fullFlight,loop)){
            // Release the manager lock before NotifyAnimationGraph takes it again.
            // MTState is an original AWU transition from jump flutter to folding.
            // Send to this wing ART only; never Actor::NotifyAnimationGraph.
            const bool accepted=model->NotifyAnimationGraph("MTState");
            SKSE::log::info("AWU grounded wing recovery: replayed wing-only MTState accepted={} (physical support, flutter state8)",accepted);
        }
    }
    std::erase_if(groundRecovery_,[&](const auto& item){return !current.contains(item.first);});
}
}
