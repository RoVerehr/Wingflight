#include "wing_audio.hpp"
#include "wing_models.hpp"
#include "wings.hpp"
#include "logging.hpp"
#include "effect_owner.hpp"
#include "awu_compatibility.hpp"
#include "vampire_runtime.hpp"
#include "continuous_wing_rate.hpp"
#include "actor_flight.hpp"
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
    // Model-less continuous flight must not inspect or animate unrelated effect ART.
    if(match.profile&&match.profile->animationless){clearContinuousBinding();return;}
    if(registeredWing(match.profile)){
        if(match.profile->continuousFlight&&active&&registered_.prepare(match).found){registered_.blend(0,1,0);registered_.loopRate(highSpeed?fastMultiplier:1.f,1);}
        return;
    }
    if(!active||!match.profile||!match.profile->continuousFlight||!wingEventActor(match.owner,match.effect)){clearContinuousBinding();return;}
    std::array<RE::BSTSmartPointer<RE::BShkbAnimationGraph>,16> graphs;
    std::array<RE::NiPointer<RE::NiAVObject>,16> roots;
    unsigned count{};
    std::unordered_set<const RE::ModelReferenceEffect*> currentModels;
    const auto stateTable=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    for(const auto& owned:wingModels(match)) {
        const auto model=owned.get();
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
            SKSE::log::debug("Continuous wing full-cycle request: adapter={} previousState={} requestedState=2 event=JumpStandingStart accepted={} (wing ART only)",match.profile->id,wingState,accepted);
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
        if(previous!=rate)SKSE::log::debug("Continuous wing frequency changed: adapter={} playbackSpeed={:.2f} highSpeed={} FastFlapPlaybackMultiplier={:.2f}",match.profile->id,rate,highSpeed,fastMultiplier);
        return;
    }
    auto next=std::make_shared<ContinuousWingBinding>();next->graphs=std::move(graphs);next->roots=std::move(roots);next->count=count;next->rate=rate;
    if(auto old=continuousBinding.exchange(next))old->rate=0;
    SKSE::log::debug("Continuous wing animation registered: adapter={} ownedGraphs={} playbackSpeed={:.2f}; jump rate in flight including Descend, sprint uses FastFlapPlaybackMultiplier",match.profile->id,count,rate);
}
float continuousWingClipRate(const RE::hkbClipGenerator* clip,const RE::hkbContext& context){
    if(!clip||!context.character)return 0;
    if(const float actorRate=actorContinuousWingRate(clip,context);actorRate>0)return actorRate;
    const auto state=continuousBinding.load();if(!state)return 0;
    const float rate=state->rate.load();if(rate<=0)return 0;
    bool owned=false;for(unsigned i=0;i<state->count;++i)if(&state->graphs[i]->characterInstance==context.character){owned=true;break;}
    if(!owned)return 0;
    if(!clip->name.c_str()||!continuousWingClip(clip->name.c_str(),clip->mode==RE::hkbClipGenerator::kModeLooping)){
        diagnostic::report("continuous.clip-layout-unrecognized","retain provider animation rate",clip,clip->name.c_str()?clip->name.c_str():"<unnamed>");return 0;}
    return rate;
}
void observeContinuousWingClip(const RE::hkbClipGenerator* clip,float before,float dt,float rate){
    if(logging::detailed()&&clip&&rate>0&&continuousRateSamples.fetch_add(1)<12)
        SKSE::log::debug("Continuous wing rate applied: clip={} flightRate={:.2f} restoredProviderRate={:.2f} timeBefore={:.4f} timeAfter={:.4f} nativeStep={:.4f} (wing graph only)",
            clip->name.c_str(),rate,clip->playbackSpeed,before,clip->localTime,dt);
}
bool Wings::initialize(float amplitude){artReady_=runtime_.install(amplitude);return artReady_;}
void Wings::advanceVisual(float delta){runtime_.advance(delta);registered_.advance(delta);advanceVampireWings(delta);if(logicalActive_)logical_.advance(delta);}
void Wings::clearVisual(bool smooth){clearContinuousBinding();strokeSound_.reset();fastSound_.reset();flutterSound_.reset();runtime_.release(smooth);registered_.release(smooth);releaseVampireWings(smooth);nativeActive_=logicalActive_=registeredActive_=false;logical_.reset();}
WingPlayback Wings::prepareVisual(WingMatch match){
    if(registeredWing(match.profile)){
        if(!registeredActive_){runtime_.release(false);releaseVampireWings(false);logicalActive_=nativeActive_=false;registeredActive_=true;}
        return registered_.prepare(match);
    }
    if(registeredActive_){registered_.release(false);registeredActive_=false;}
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
        if(!wingEventActor(match.owner,match.effect))return out;
        for(const auto& owned:wingModels(match)) {
            const auto model=owned.get();
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
    if(registeredWing(match.profile))return registered_.blend(flap,ownership,fast);
    if(match.profile&&match.profile->animationless) {
        if(!wingEventActor(match.owner,match.effect))return false;
        const_cast<Wings*>(this)->logical_.blend(flap,ownership,fast);return true;
    }
    if(match.profile&&match.profile->continuousFlight)return wingEventActor(match.owner,match.effect)!=nullptr;
    if(match.profile&&match.profile->nativeVampire)return blendVampireWings(flap,ownership,fast);
    if(!match.profile||!wingEventActor(match.owner,match.effect))return false;
    return const_cast<WingRuntime&>(runtime_).blend(flap,ownership,fast);
}
bool Wings::parkInvisibleFlap(){return registeredActive_?registered_.parkPower():logicalActive_?logical_.parkPower():nativeActive_?parkVampirePower():runtime_.parkPower();}
bool Wings::setJumpPreview(WingMatch match,float amount,float period,float amplitude){
    if(!match.profile||!match.profile->awuCompatibility||match.profile->nativeVampire||match.profile->continuousFlight||match.profile->animationless)return false;
    return prepareVisual(match).found&&runtime_.preview(amount,period,amplitude);
}
void Wings::playJumpOpeningSound(WingMatch match){
    if(!match.profile||!match.profile->awuCompatibility||!wingSoundActor(match.owner,match.effect,false)
        ||match.owner->AsActorState()->IsSwimming())return;
    const auto found=sounds_.find(match.profile);
    auto descriptor=found==sounds_.end()?nullptr:found->second;
    if(!descriptor&&match.profile->flapSoundFiles.empty())return;
    RE::BSSoundHandle handle;
    const bool built=buildWingSound(handle,descriptor,match.profile->flapSoundFiles,0x1A2FF);
    bool played=false;
    if(built){handle.SetPosition(match.owner->GetPosition());if(auto node=match.owner->Get3D())handle.SetObjectToFollow(node);played=handle.Play();}
    SKSE::log::debug("Jump wing opening sound: built={} played={}",built,played);
    if(!built||!played)diagnostic::report(!built?"sound.handle-build-failed":"sound.play-failed","skip jump opening sound",match.owner,"jump-opening");
}
bool Wings::parkInvisibleGlide(){return registeredActive_?registered_.parkFast():logicalActive_?logical_.parkFast():nativeActive_?parkVampireFast():runtime_.parkFast();}
void Wings::setFlapRate(float rate){if(registeredActive_)registered_.powerRate(rate);else if(logicalActive_)logical_.powerRate(rate);else if(nativeActive_)rateVampireWings(rate);else runtime_.powerRate(rate);}
void Wings::updateGlideRate(float fast,float spread){if(registeredActive_)registered_.loopRate(fast,spread);else if(logicalActive_)logical_.loopRate(fast,spread);else if(nativeActive_)loopRateVampireWings(fast,spread);else runtime_.loopRate(fast,spread);}
void Wings::updatePowerPitch(float degrees){if(registeredActive_){registered_.pitch(degrees);return;}if(logicalActive_)return;if(nativeActive_)pitchVampireWings(degrees);else runtime_.pitch(degrees);}
void Wings::resolve(const Configuration& cfg) {
    resetWingModelCache();
    loadWingRegistrations();
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
        const bool optionalSoundMissing=profile.soundPlugin!=profile.plugin
            &&!data->LookupLoadedModByName(profile.soundPlugin)&&!data->LookupLoadedLightModByName(profile.soundPlugin);
        if(optionalSoundMissing)SKSE::log::info("Optional sound provider {} absent for {}; wing audio disabled",profile.soundPlugin,profile.id);
        if(profile.flapSoundID&&!optionalSoundMissing) {
            if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(profile.flapSoundID,profile.soundPlugin)) {
                sounds_.emplace(&profile,sound);
            } else { SKSE::log::warn("Missing power-stroke sound {}:{:06X}",profile.soundPlugin,profile.flapSoundID); }
        }
        if(profile.fastSoundID&&!optionalSoundMissing) {
            if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(profile.fastSoundID,profile.soundPlugin))fastSounds_.emplace(&profile,sound);
            else SKSE::log::warn("Missing fast-flap sound {}:{:06X}",profile.plugin,profile.fastSoundID);
        }
        if(profile.spreadSoundID&&profile.spreadSoundVolume>0&&!optionalSoundMissing) {
            if(auto sound=data->LookupForm<RE::BGSSoundDescriptorForm>(profile.spreadSoundID,profile.soundPlugin))spreadSounds_.emplace(&profile,sound);
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
    if(match.profile&&!match.profile->soundPlugin.empty()&&match.profile->soundPlugin!=match.profile->plugin
        &&match.profile->flapSoundFiles.empty()&&match.profile->fastSoundFiles.empty()&&match.profile->spreadSoundFiles.empty()){
        const auto data=RE::TESDataHandler::GetSingleton();
        if(!data->LookupLoadedModByName(match.profile->soundPlugin)&&!data->LookupLoadedLightModByName(match.profile->soundPlugin)){
            strokeSound_.reset();fastSound_.reset();flutterSound_.reset();return;
        }
    }
    if(registeredWing(match.profile)){
        auto resolve=[&](auto& map,std::uint32_t id){if(id&&!map.contains(match.profile))if(auto sound=RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSSoundDescriptorForm>(id,match.profile->soundPlugin))map.emplace(match.profile,sound);};
        resolve(sounds_,match.profile->flapSoundID);resolve(fastSounds_,match.profile->fastSoundID);resolve(spreadSounds_,match.profile->spreadSoundID);
    }
    if(match.profile&&match.profile->continuousFlight){strokeSound_.reset();fastSound_.reset();flutterSound_.reset();return;}
    const bool effectless=registeredWing(match.profile)&&(!match.profile->armorIDs.empty()||match.profile->animationless);
    if(!match.profile||(match.profile->nativeVampire?!isNativeVampireLord(match.owner):!wingSoundActor(match.owner,match.effect,effectless))){strokeSound_.reset();fastSound_.reset();flutterSound_.reset();return;}
    const auto& profile=*match.profile;
    const bool power=strokeSound_.update(playback.found&&playback.active,playback.phase,
        playback.flapWeight,playback.frequency,profile.flapSoundPhase,profile.timing.downstrokeEnd);
    const bool fast=fastSound_.update(playback.found&&playback.gliding,playback.glidePhase,
        playback.glideWeight,playback.glideFrequency,profile.fastSoundPhase,1.0f);
    const bool flutter=flutterSound_.update(playback.found&&playback.spreadActive,playback.spreadPhase,
        playback.spreadWeight,playback.spreadFrequency,profile.spreadSoundPhase1,profile.spreadSoundPhase2);
    auto play=[&](const auto& descriptors,const auto& files,const char* kind,float phase,std::uint32_t output,float volume=1.0f){
        if(volume<=0)return;
        const auto found=descriptors.find(match.profile);
        auto descriptor=found==descriptors.end()?nullptr:found->second;
        if(!descriptor&&files.empty()){
            const bool configured=std::string_view(kind)=="power-stroke"?profile.flapSoundID!=0
                :std::string_view(kind)=="fast-flap"?profile.fastSoundID!=0:profile.spreadSoundID!=0&&profile.spreadSoundVolume>0;
            if(configured)diagnostic::report("sound.descriptor-unresolved","skip sound playback",match.owner,kind);
            return;
        }
        RE::BSSoundHandle handle;const bool built=buildWingSound(handle,descriptor,files,output);bool played=false;
        if(built){handle.SetPosition(match.owner->GetPosition());if(auto node=match.owner->Get3D())handle.SetObjectToFollow(node);
            if(volume!=1.0f)handle.SetVolume(volume);played=handle.Play();}
        SKSE::log::debug("Runtime {} sound: phase={:.4f} built={} played={} source={} descriptor={:08X}",kind,phase,built,played,files.empty()?"descriptor":"file",descriptor?descriptor->GetFormID():0);
        if(!built||!played)diagnostic::report(!built?"sound.handle-build-failed":"sound.play-failed","skip this playback",match.owner,kind);
    };
    if(power)play(sounds_,profile.flapSoundFiles,"power-stroke",playback.phase,0x1A2FF);
    if(fast)play(fastSounds_,profile.fastSoundFiles,"fast-flap",playback.glidePhase,0x5A28A);
    if(flutter)play(spreadSounds_,profile.spreadSoundFiles,"flutter-glide",playback.spreadPhase,0x5A28A,
        profile.spreadSoundVolume*std::clamp(playback.spreadWeight,0.0f,1.0f));
}
bool Wings::hasWingSource(RE::Actor* actor)const{
    if(!actor)return false;
    if(hasRegisteredWingSource(actor)||isNativeVampireLord(actor))return true;
    if(const auto list=actor->AsMagicTarget()->GetActiveEffectList())for(const auto effect:*list)
        if(effect&&effect->target==actor->AsMagicTarget()
            &&!effect->flags.any(RE::ActiveEffect::Flag::kInactive,RE::ActiveEffect::Flag::kDispelled)
            &&effects_.contains(effect->GetBaseObject()))return true;
    return false;
}
WingMatch Wings::find(RE::Actor* actor,bool ownedFlight,bool appearanceOnly) const {
    if (!actor) { return {}; }
    // Body graph decides stance/events. A visible AWU ART is independent of
    // whether the player currently uses a VampireLord or Character body graph.
    if(!appearanceOnly&&usesVampireLordBehavior(actor)){
        bool stance{};
        if(!actor->GetGraphVariableBool("isLevitating",stance)||(!stance&&!ownedFlight))return {};
    }
    if(const auto registered=findRegisteredWings(actor);registered.profile)return registered;
    WingMatch art,invisible;
    const auto active = actor->AsMagicTarget()->GetActiveEffectList();
    if (active&&artReady_)for (auto effect : *active) {
        if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) { continue; }
        const auto found = effects_.find(effect->GetBaseObject());
        if (found != effects_.end()) {
            // AWU's Jump input listener is player-only. NPCs use the same
            // visible-effect/provider binding without waiting for player input.
            if(!appearanceOnly && found->second->awuCompatibility && actor==RE::PlayerCharacter::GetSingleton() && !awuEffectReady(actor,effect))continue;
            if(found->second->animationless) {
                if(wingEventActor(actor,effect))invisible={found->second,effect,actor};
                continue;
            }
            if(!wingEventActor(actor,effect))continue;
            const bool attached=!wingModels({found->second,effect,actor}).empty();
            if(attached){art={found->second,effect,actor};break;}
        }
    }
    const bool skeletal=appearanceOnly?nativeVampireAppearanceEligible(actor):(nativeVampireFlightEligible(actor,ownedFlight)&&vampireSceneReady());
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
    if(registeredWing(match.profile))return event==match.profile->land||event==match.profile->fall||const_cast<Wings*>(this)->prepareVisual(match).found;
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
    // Borrow only validated wing ART models. Script-generated providers opt in
    // through explicit ArtObjects; unrelated provider models are left alone.
    if(!nativeWingControls(match.profile)||!wingEventActor(match.owner,match.effect)){groundRecovery_.clear();return;}
    std::unordered_set<const RE::ModelReferenceEffect*> current;
    const auto stateTable=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    for(const auto& owned:wingModels(match)){
        auto* model=owned.get();
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
            // Keep AWU MTState; DAO-style graphs fold through JumpLandEnd.
            // Send to this wing ART only; never Actor::NotifyAnimationGraph.
            const auto command=wingFoldEvent(match.profile);
            const bool accepted=model->NotifyAnimationGraph(command);
            SKSE::log::debug("Grounded wing recovery: wing-only {} accepted={} adapter={} (physical support, flutter state8)",command,accepted,match.profile->id);
        }
    }
    std::erase_if(groundRecovery_,[&](const auto& item){return !current.contains(item.first);});
}
}
