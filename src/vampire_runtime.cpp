#include "vampire_runtime.hpp"
#include "actor_state.hpp"
#include "logging.hpp"
#include "vampire_animation.hpp"
#include "vampire_policy.hpp"
#include "vampire_geometry.hpp"
#include "vampire_resource.hpp"
#include "wing_runtime_policy.hpp"
#include "armed_bank.hpp"
#include "wings.hpp"
#include "runtime_support.hpp"
#include <SKSE/SKSE.h>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace wingflight {
namespace {
RE::TESRace* nativeRace{};
RE::BGSKeyword* vlrnRaceKeyword{};
RE::BGSListForm *vlrnSelectedWings{},*vlrnNativeWings{};
RE::TESGlobal* vlrnBeastAppearance{};
std::atomic_uint64_t context{}; // race ID + independent body/visibility bits
vampire::Driver requestedDriver{vampire::Driver::automatic};
bool enabled=true,sceneInstalled{},resourcesReady{},resourceFailure{};
float amplitude{1},flutterCycleSeconds{0.60f};
std::array<vampire::Animation,3> sourceAnimations;
std::array<std::array<std::size_t,10>,3> tracks;
std::mutex resourceLock,sceneLock;
using SceneMap=std::unordered_map<const RE::NiAVObject*,std::weak_ptr<VampireWingState>>;
std::atomic<std::shared_ptr<const SceneMap>> scenes;
legs::Quaternion rotation(const RE::NiMatrix3& m){legs::Matrix x{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)x[i][j]=m.entry[i][j];return legs::quaternion(x);}
void rotation(RE::NiMatrix3& m,legs::Quaternion q){auto x=legs::matrix(q);for(int i=0;i<3;++i)for(int j=0;j<3;++j)m.entry[i][j]=x[i][j];}
bool graphOf(const RE::Actor* actor){
    if(!actor)return false;RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if(!actor->GetAnimationGraphManager(manager)||!manager)return false;
    RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);
    for(auto& g:manager->graphs)if(g&&g->holder==actor&&g->projectName.c_str()
        &&legs::canonical(g->projectName.c_str())=="vampirelord")return true;
    return false;
}
std::uint64_t inspectVampireActor(const RE::Actor* a){
    std::uint64_t next=0;
    if(a&&a->Is3DLoaded()&&!a->IsDead()&&!knockedDown(a)&&a->GetRace()){
        auto race=a->GetRace();const bool vlrn=vlrnRaceKeyword&&race->HasKeyword(vlrnRaceKeyword);
        const bool known=race==nativeRace||vlrn;
        if(known){const bool nativeBody=graphOf(a);bool injected{};
            const bool humanBody=!nativeBody&&a->GetGraphVariableBool("Wingflight_IsFlying",injected);
            if(vampire::validBody(known,nativeBody,humanBody)){
                bool worn=false;
                // Examine actual worn entries; a selected but unequipped wing does
                // not grant flight. No inventory initialization or script writes.
                if(vlrn&&vlrnSelectedWings&&vlrnNativeWings){
                    if(const auto inv=const_cast<RE::Actor*>(a)->GetInventoryChanges(true);inv&&inv->entryList)
                        for(const auto e:*inv->entryList)if(e&&e->object&&e->IsWorn()
                            &&vlrnSelectedWings->HasForm(e->object)&&vlrnNativeWings->HasForm(e->object)){worn=true;break;}}
                const bool beast=nativeBody&&(a!=RE::PlayerCharacter::GetSingleton()||(vlrnBeastAppearance&&vlrnBeastAppearance->value==1));
                const bool rig=vampire::rigVisible(vlrn,beast,worn);
                next=(std::uint64_t(race->GetFormID())<<8)|1|(nativeBody?2:0)|(rig?4:0);
            }
        }
    }
    return next;
}
bool load(){
    std::lock_guard guard(resourceLock);
    if(resourcesReady)return true;if(resourceFailure)return false;
    const char* loading="Data/Skyrim - Animations.bsa";
    try{
        const auto originals=vampire::originalAssets(loading,{
            "meshes/actors/vampirelord/character assets/skeleton.hkx",
            "meshes/actors/vampirelord/animations/levitate_sprint.hkx",
            "meshes/actors/vampirelord/animations/levitate_idle.hkx"});
        const auto skeleton=vampire::Skeleton::read(originals[0]);
        for(unsigned c:{0u,2u}){sourceAnimations[c]=vampire::Animation::read(originals[c==0?1:2],skeleton);
            for(unsigned i=0;i<10;++i){auto name=std::find(skeleton.names.begin(),skeleton.names.end(),vampire::wingNames[i]);
                vampire::Packfile::require(name!=skeleton.names.end(),"native skeleton wing absent");
                auto bone=std::size_t(name-skeleton.names.begin());auto track=std::find(sourceAnimations[c].boneIndices.begin(),sourceAnimations[c].boneIndices.end(),bone);
                vampire::Packfile::require(track!=sourceAnimations[c].boneIndices.end(),"animation wing track absent");tracks[c][i]=std::size_t(track-sourceAnimations[c].boneIndices.begin());}}
        resourcesReady=true;
        SKSE::log::info("Vampire Lord wing resources: 10 bone rotations, original HKX read from Skyrim - Animations.bsa; humanoid body replacements untouched; powerSource={:.3f}s flutterSource={:.3f}s flutterCycle={:.2f}s amplitude={:.2f}; no distributed game assets",sourceAnimations[0].duration,sourceAnimations[2].duration,flutterCycleSeconds,amplitude);
        return true;
    }catch(const std::exception& e){resourceFailure=true;SKSE::log::error("Native Vampire Lord resource failed: path={} reason={}; native flight unavailable",loading,e.what());return false;}
}
std::array<legs::Quaternion,10> pose(unsigned clip,float phase){std::array<legs::Quaternion,10> q;
    for(unsigned i=0;i<10;++i)q[i]=sourceAnimations[clip].sample(tracks[clip][i],phase);return q;}
}
struct VampireWingState : vampire::PlaybackState, std::enable_shared_from_this<VampireWingState> {
    std::mutex stateLock;
    RE::ActorHandle owner;
    RE::FormID ownerID{},boundRace{};
    struct Bone {RE::NiPointer<RE::NiAVObject> node;legs::Quaternion before,output;bool wrote{};};
    RE::NiPointer<RE::NiAVObject> root;
    std::array<Bone,10> bones;
    float peak{},bottom{},extended{},downEnd{.32f},wingPitch{};
    bool paused{};unsigned samples{};
bool current(){auto actor=owner.get();return actor&&actor->GetRace()&&actor->GetRace()->GetFormID()==boundRace
    &&actor->Is3DLoaded()&&!actor->IsDead()&&!knockedDown(actor)&&!actor->IsInKillMove()&&root&&actor->Get3D(false)==root.get();}
void restore(){
    if(current())for(auto& b:bones)if(b.node&&b.wrote&&std::abs(legs::dot(rotation(b.node->local.rotate),b.output))>0.999999f)
        rotation(b.node->local.rotate,b.before);
    for(auto& b:bones)b.wrote=false;
}
void drop(){
    restore();
    if(root){std::lock_guard guard(sceneLock);const auto old=scenes.load();if(old){auto next=std::make_shared<SceneMap>(*old);
        if(const auto it=next->find(root.get());it!=next->end()&&it->second.lock().get()==this)next->erase(it);
        scenes.store(next);}}
    root.reset();owner={};ownerID=boundRace=0;bones={};static_cast<vampire::PlaybackState&>(*this)={};samples=0;wingPitch=0;
}
void choosePhases(){
    // Height/span are evaluated in the wing parent's local coordinates, so the
    // character's current body tilt and camera direction cannot affect phases.
    RE::NiAVObject* anchor=nullptr;
    unsigned leftDepth=0;
    for(auto* left=bones[0].node->parent;left&&!anchor&&leftDepth++<64;left=left->parent){
        unsigned depth=0;for(auto* right=bones[5].node->parent;right&&depth++<64;right=right->parent)
            if(right==left){anchor=left;break;}
    }
    vampire::Packfile::require(anchor!=nullptr,"wing roots have no shared parent");
    std::array<std::vector<vampire::PathNode>,2> paths;
    for(unsigned side=0;side<2;++side){
        auto* node=bones[side*5+4].node.get();unsigned depth=0;
        while(node&&node!=anchor&&depth++<64){
            unsigned keyed=10;for(unsigned i=0;i<10;++i)if(bones[i].node.get()==node){keyed=i;break;}
            paths[side].push_back({{node->local.translate.x,node->local.translate.y,node->local.translate.z},rotation(node->local.rotate),node->local.scale,keyed});
            node=node->parent;
        }
        vampire::Packfile::require(node==anchor,"wing parent chain exceeds limit");
        std::reverse(paths[side].begin(),paths[side].end());
    }
    SKSE::log::debug("Native Vampire Lord wing geometry: leftPath={} rightPath={} nodes; installed proxy transforms, translations and scales preserved",paths[0].size(),paths[1].size());
    float high=-1e9f,low=1e9f,wide=-1e9f;
    for(unsigned f=0;f<120;++f){const float phase=float(f)/120;const auto q=pose(0,phase);
        const auto left=vampire::tip(paths[0],q),right=vampire::tip(paths[1],q);
        const float height=(left[2]+right[2])*0.5f;
        const float span=std::abs(left[0]-right[0]);
        if(height>high){high=height;peak=phase;}if(height<low){low=height;bottom=phase;}
        if(span>wide){wide=span;extended=phase;}
    }
    const float arc=std::fmod(bottom-peak+1,1.0f);
    vampire::Packfile::require(high-low>1&&arc>0.05f&&arc<0.95f,"native wing geometry phase range");
    SKSE::log::debug("Native Vampire Lord wing phases: peak={:.3f} bottom={:.3f} spread={:.3f} heightSpan={:.2f} lateralSpan={:.2f}",peak,bottom,extended,high-low,wide);
}
WingPlayback playback(){
    if(!root||!resourcesReady)return {};
    return {.found=true,.active=true,.gliding=true,.spreadActive=true,
        .spreadWeight=displayed.spread,.graphSpreadWeight=displayed.spread,
        .spreadPhase=clocks[2].phase,.spreadFrequency=clocks[2].rate,
        .phase=clocks[0].phase,.frequency=clocks[0].rate,.duration=sourceAnimations[0].duration,
        .flapWeight=displayed.flap,.glideWeight=displayed.fast,.graphWeight=displayed.flap,
        .otherWeight=displayed.original,.graphGlideWeight=displayed.fast,.glidePhase=clocks[1].phase,
        .glideFrequency=clocks[1].rate,.activeSequence="Native Vampire Lord wing-only sampler"};
}
WingPlayback prepare(RE::Actor* actor){
    if(!sceneInstalled||!actor||!actor->GetRace()||actor->IsDead()||knockedDown(actor))return {};
    std::lock_guard guard(stateLock);if(!load())return {};
    auto* object=actor->Get3D(false);if(!object)return {};
    if(root.get()!=object||ownerID!=actor->GetFormID()||boundRace!=actor->GetRace()->GetFormID()){
        drop();root.reset(object);owner=actor->GetHandle();ownerID=actor->GetFormID();boundRace=actor->GetRace()->GetFormID();
        for(unsigned i=0;i<10;++i){bones[i].node.reset(object->GetObjectByName(vampire::wingNames[i]));if(!bones[i].node){
            SKSE::log::error("Native Vampire Lord skeleton missing wing: {}; native flight unavailable",vampire::wingNames[i]);drop();return {};}}
        try{choosePhases();}catch(const std::exception& e){SKSE::log::error("Native Vampire Lord phase selection failed: {}",e.what());drop();return {};}
        clocks[0].duration=clocks[1].duration=sourceAnimations[0].duration;
        clocks[2].duration=flutterCycleSeconds;
        clocks[0].rate=0.01f;desired.original=displayed.original=1;
        {std::lock_guard sceneGuard(sceneLock);const auto old=scenes.load();auto next=std::make_shared<SceneMap>(old?*old:SceneMap{});(*next)[object]=weak_from_this();scenes.store(next);}
        SKSE::log::debug("Native Vampire Lord wing driver bound: actor={:08X} root={:X}; independent ten-bone sampler",ownerID,reinterpret_cast<std::uintptr_t>(object));
    }
    return playback();
}
void advance(float dt){std::lock_guard guard(stateLock);if(!root)return;if(!current()){drop();return;}
    advancePlayback(dt);if(released&&displayed.original>0.9995f)drop();}

bool blend(float flap,float ownership,float fast){std::lock_guard guard(stateLock);if(!root)return false;
    (void)fast;desired=vampire::nativeWeights(flap,ownership);
    released=ownership<=0;return true;}
void rate(float r){std::lock_guard guard(stateLock);if(std::isfinite(r)&&r>=0)clocks[0].rate=r;}
void loopRate(float fast,float spread){std::lock_guard guard(stateLock);clocks[1].rate=glidePlaybackScale(displayed.fast,fast);clocks[2].rate=glidePlaybackScale(displayed.spread,spread);}
void pitch(float p){std::lock_guard guard(stateLock);wingPitch=std::isfinite(p)?std::clamp(p,0.0f,90.0f):0;}
bool parkPower(){std::lock_guard guard(stateLock);if(!root||displayed.flap>0.0001f)return false;clocks[0].phase=0;clocks[0].rate=0.01f;return true;}
bool parkFast(){std::lock_guard guard(stateLock);if(!root||displayed.fast>0.0001f)return false;clocks[1].phase=0;return true;}
void release(bool smooth){std::lock_guard guard(stateLock);if(smooth&&current()){desired={0,0,0,1};released=true;}else drop();}
bool apply(RE::NiAVObject* object,bool postEvaluation){
    std::lock_guard guard(stateLock);if(!root||object!=root.get())return false;
    if(!current()){drop();return false;}
    if(paused){restore();return false;} // Native bat pose/root motion retain ownership.
    if(applied==requested){
        bool changed=false;
        if(postEvaluation)for(auto& b:bones)if(b.node&&b.wrote){const auto native=rotation(b.node->local.rotate);
            if(std::abs(legs::dot(native,b.output))<.999999f){b.before=native;rotation(b.node->local.rotate,b.output);changed=true;}}
        return changed;
    }
    applied=requested;
    if(displayed.original>=0.99999f)return false;
    const auto power=pose(0,vampire::sourcePhase(clocks[0].phase,peak,bottom,downEnd));
    const auto spread=pose(0,extended);
    const auto flutter=pose(2,clocks[2].phase),center=pose(2,0);
    for(unsigned i=0;i<10;++i){auto& b=bones[i];auto native=rotation(b.node->local.rotate);
        if(b.wrote&&std::abs(legs::dot(native,b.output))>0.999999f)native=b.before;
        b.before=native;
        // Keep the spread center fixed while layering a small native flutter;
        // root attachments/translation/scale remain those of the installed NIF.
        const auto flutterPose=vampire::flutterPose(spread[i],center[i],flutter[i],amplitude);
        const float wing=displayed.flap+displayed.fast+displayed.spread;if(wing<=0)continue;
        auto target=legs::blend(power[i],flutterPose,displayed.spread/wing);
        // Direction correction excludes the attachment bone of each wing.
        if(i%5==1||i%5==2){const float part=i%5==1?0.25f:0.75f;
            RE::NiMatrix3 basis;auto p=b.node->parent;unsigned depth=0;
            while(p&&p!=object&&depth++<64){basis=p->local.rotate*basis;p=p->parent;}
            if(p==object){auto axis=basis.Transpose()*RE::NiPoint3(1,0,0);
                target=legs::bankAroundAxis(target,-wingPitch*part*displayed.flap,axis.x,axis.y,axis.z);}}
        const auto wanted=legs::blend(native,target,wing);
        // First frame starts from the actually displayed native pose. Subsequent
        // target reversals retain the current output instead of seeking a frame.
        const auto previous=b.wrote?b.output:native;
        b.output=legs::blend(previous,wanted,1-std::exp(-delta*35));b.wrote=true;
        rotation(b.node->local.rotate,b.output);
    }
    if(logging::detailed()&&samples++<8)SKSE::log::debug("Native Vampire Lord wing-only pose applied: actor={:08X} phase={:.3f} flap={:.3f} fast={:.3f} spread={:.3f} original={:.3f}",ownerID,clocks[0].phase,displayed.flap,displayed.fast,displayed.spread,displayed.original);
    return true;
}
};
void resolveVampireFlight(const Configuration& c){
    amplitude=c.spreadAmplitudeMultiplier;
    const auto path=std::filesystem::absolute("Data/SKSE/Plugins/Wingflight/VampireLord.ini").string();
    enabled=GetPrivateProfileIntA("VampireLord","Enabled",1,path.c_str())!=0;
    char cycle[64]{};GetPrivateProfileStringA("VampireLord","FlutterCycleSeconds","0.60",cycle,sizeof(cycle),path.c_str());
    char* end{};const float value=std::strtof(cycle,&end);
    if(end!=cycle&&!*end&&std::isfinite(value)&&value>=0.15f&&value<=3)flutterCycleSeconds=value;
    else SKSE::log::warn("Native Vampire Lord FlutterCycleSeconds invalid: '{}'; using {:.2f}s (range 0.15..3.00)",cycle,flutterCycleSeconds);
    nativeRace=RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESRace>(0x283a,"Dawnguard.esm");
    const auto data=RE::TESDataHandler::GetSingleton();
    vlrnRaceKeyword=data->LookupForm<RE::BGSKeyword>(0x831,"VLrn Patch 00.esp");
    vlrnSelectedWings=data->LookupForm<RE::BGSListForm>(0x826,"VLrn Patch 00.esp");
    vlrnNativeWings=data->LookupForm<RE::BGSListForm>(0x824,"VLrn Patch 00.esp");
    vlrnBeastAppearance=data->LookupForm<RE::TESGlobal>(0x840,"VLrn Patch 00.esp");
    if(data->LookupLoadedModByName("VLrn Patch 00.esp")||data->LookupLoadedLightModByName("VLrn Patch 00.esp")){
        if(!vlrnRaceKeyword||!vlrnSelectedWings||!vlrnNativeWings||!vlrnBeastAppearance)
            SKSE::log::warn("VLrn compatibility record missing: raceKeyword={} selectedWings={} nativeWingList={} beastAppearance={}; missing worn-wing records do not grant skeletal flight",bool(vlrnRaceKeyword),bool(vlrnSelectedWings),bool(vlrnNativeWings),bool(vlrnBeastAppearance));
    }
    char driver[32]{};GetPrivateProfileStringA("VampireLord","WingDriver","Auto",driver,sizeof(driver),path.c_str());
    const auto name=legs::canonical(driver);requestedDriver=vampire::Driver::automatic;
    if(name=="native")requestedDriver=vampire::Driver::skeleton;
    else if(name=="effect")requestedDriver=vampire::Driver::effect;
    else if(name!="auto")SKSE::log::warn("Vampire Lord WingDriver invalid: '{}'; using Auto (Auto/Native/Effect)",driver);
    context=0;
    SKSE::log::info("Vampire Lord providers: enabled={} nativeRace={:08X} VLrn={} selectedWings={} nativeWingList={} appearanceOption={} wingDriver={}; body graph and visible wings selected independently",enabled,nativeRace?nativeRace->GetFormID():0,bool(vlrnRaceKeyword),bool(vlrnSelectedWings),bool(vlrnNativeWings),bool(vlrnBeastAppearance),driver);
}
// Safe inside graph callbacks: no graph-manager lock is acquired here. The
// precise beast project is checked by eligibility before binding/starting.
bool isNativeVampireLord(const RE::Actor* a){const auto c=context.load();return a&&a==RE::PlayerCharacter::GetSingleton()&&a->GetRace()&&(c>>8)==a->GetRace()->GetFormID()&&(c&1);}
bool usesVampireLordBehavior(const RE::Actor* a){return isNativeVampireLord(a)&&(context.load()&2);}
vampire::Driver vampireWingDriver(){return requestedDriver;}
void refreshVampireContext(RE::PlayerCharacter* a){
    const auto next=inspectVampireActor(a);
    const auto old=context.exchange(next);
    if(old!=next&&(old||next))SKSE::log::debug("Vampire Lord context: race={:08X} body={} skeletalWingsVisible={}; eligibility follows actual transformation/equipment",next>>8,next&2?"VampireLord":next&1?"Character":"none",bool(next&4));
}
bool nativeVampireFlightEligible(const RE::Actor* a,bool ownedFlight){
    if(a&&a!=RE::PlayerCharacter::GetSingleton()){
        const auto info=inspectVampireActor(a);
        if(!enabled||!(info&1)||!(info&4)||a->IsInKillMove())return false;
        const auto body=a->Get3D(false);if(!body)return false;
        for(const auto name:vampire::wingNames)if(!body->GetObjectByName(name))return false;
        if(info&2){bool levitating{};const bool readable=a->GetGraphVariableBool("isLevitating",levitating);
            if(!vampire::followerStancePermitsFlight(readable,levitating,ownedFlight,a->IsInCombat()))return false;}
        return true;
    }
    if(!enabled||!isNativeVampireLord(a)||!(context.load()&4)||a->IsInKillMove())return false;
    // Bone presence/binding is validated at prepare before any flight starts.
    if(!vampire::eligible(enabled,true,a->Is3DLoaded(),!a->IsDead(),knockedDown(a),true,true))return false;
    if(!usesVampireLordBehavior(a))return true; // Character graph has no native levitation stance.
    bool levitating{};const bool readable=a->GetGraphVariableBool("isLevitating",levitating);
    static unsigned stanceSamples{};
    if(logging::detailed()&&ownedFlight&&readable&&!levitating&&stanceSamples++<8)
        SKSE::log::debug("Native Vampire Lord transient stance: isLevitating=false, retaining owned flight; race/project/actor remain valid");
    if(!readable)diagnostic::report("vampire.levitation-variable-missing","reject native flight provider",a,"isLevitating");
    return vampire::stancePermitsFlight(readable,levitating,ownedFlight);
}
namespace {VampireWingRuntime playerWings;}
VampireWingRuntime::~VampireWingRuntime(){release(false);}
WingPlayback VampireWingRuntime::prepare(RE::Actor* actor){if(!state_)state_=std::make_shared<VampireWingState>();return state_->prepare(actor);}
void VampireWingRuntime::advance(float dt){if(state_)state_->advance(dt);}
bool VampireWingRuntime::blend(float flap,float ownership,float fast){return state_&&state_->blend(flap,ownership,fast);}
void VampireWingRuntime::rate(float value){if(state_)state_->rate(value);}
void VampireWingRuntime::loopRate(float fast,float spread){if(state_)state_->loopRate(fast,spread);}
void VampireWingRuntime::pitch(float value){if(state_)state_->pitch(value);}
bool VampireWingRuntime::parkPower(){return state_&&state_->parkPower();}
bool VampireWingRuntime::parkFast(){return state_&&state_->parkFast();}
void VampireWingRuntime::release(bool smooth){if(state_)state_->release(smooth);}
void VampireWingRuntime::pause(bool value){if(state_){std::lock_guard guard(state_->stateLock);state_->paused=value;}}
bool inspectVampireBody(const RE::Actor* actor){return graphOf(actor);}
vampire::LevitationState inspectVampireLevitation(const RE::Actor* actor){
    using vampire::LevitationState;
    if(!actor)return LevitationState::unavailable;
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if(!actor->GetAnimationGraphManager(manager)||!manager)return LevitationState::unavailable;
    const auto table=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);
    for(const auto& graph:manager->graphs){
        if(!graph||graph->holder!=actor||!graph->projectName.c_str()
            ||legs::canonical(graph->projectName.c_str())!="vampirelord")continue;
        const auto behavior=graph->characterInstance.behaviorGraph;
        if(!behavior||!behavior->activeNodes||behavior->activeNodes->size()>512)continue;
        for(const auto& info:*behavior->activeNodes){
            const auto node=info.nodeClone;
            if(!node||*reinterpret_cast<const std::uintptr_t*>(node)!=table
                ||!node->name.c_str()||std::string_view(node->name.c_str())!="H2HBehavior")continue;
            const auto machine=static_cast<const RE::hkbStateMachine*>(node);
            if(machine->isActive)return vampire::levitationState(machine->currentStateID,!machine->activeTransitions.empty());
        }
    }
    return LevitationState::unavailable;
}
WingPlayback prepareVampireWings(RE::Actor* actor){
    if(!isNativeVampireLord(actor)||actor!=RE::PlayerCharacter::GetSingleton())return {};
    return playerWings.prepare(actor);
}
void advanceVampireWings(float dt){playerWings.advance(dt);}
bool blendVampireWings(float flap,float ownership,float fast){return playerWings.blend(flap,ownership,fast);}
void rateVampireWings(float value){playerWings.rate(value);}
void loopRateVampireWings(float fast,float spread){playerWings.loopRate(fast,spread);}
void pitchVampireWings(float value){playerWings.pitch(value);}
bool parkVampirePower(){return playerWings.parkPower();}
bool parkVampireFast(){return playerWings.parkFast();}
void releaseVampireWings(bool smooth){playerWings.release(smooth);}
void applyVampireWings(RE::NiAVObject* object,RE::NiUpdateData* data,bool postEvaluation){
    const auto map=scenes.load();if(!map||map->empty()||!object)return;
    std::shared_ptr<VampireWingState> state;auto ancestor=object;
    for(unsigned depth=0;ancestor&&depth<16;++depth,ancestor=ancestor->parent){
        const auto found=map->find(ancestor);if(found!=map->end()){state=found->second.lock();break;}}
    if(!state)return;
    if(!state->apply(ancestor,postEvaluation)||!postEvaluation||!data)return;
    std::lock_guard guard(state->stateLock);if(!state->current()||state->root.get()!=ancestor)return;
    auto refresh=[&](auto&& self,RE::NiAVObject* node,unsigned depth)->void{
        if(!node||depth>32)return;node->UpdateWorldData(data);
        if(const auto branch=node->AsNode())for(const auto& child:branch->GetChildren())if(child)self(self,child.get(),depth+1);
    };
    for(unsigned i:{0u,5u})if(state->bones[i].node)refresh(refresh,state->bones[i].node.get(),0);
    if(const auto tree=netimmerse_cast<RE::BSFlattenedBoneTree*>(object)){
        auto& cache=tree->GetRuntimeData();if(!cache.boneEntries||cache.numBones>2048)return;
        for(unsigned i=0;i<cache.numBones;++i){auto& bone=cache.boneEntries[i];
            if(bone.node){bone.local=bone.node->local;bone.world=bone.node->world;}
            else if(bone.parentIndex>=0&&static_cast<unsigned>(bone.parentIndex)<i)bone.world=cache.boneEntries[bone.parentIndex].world*bone.local;}
    }
}
bool vampireSceneReady(){return sceneInstalled;}
void pauseVampireWingOverlay(bool value){playerWings.pause(value);}
bool nativeVampireJumpEvent(const char* event){return event&&vampire::jumpRequest(event);}
bool installVampireScene(){sceneInstalled=true;return true;}
}
