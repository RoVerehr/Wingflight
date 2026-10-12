#include "vampire_runtime.hpp"
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

namespace wingflight {
namespace {
std::mutex stateLock;
RE::TESRace* nativeRace{};
RE::BGSKeyword* vlrnRaceKeyword{};
RE::BGSListForm *vlrnSelectedWings{},*vlrnNativeWings{};
RE::TESGlobal* vlrnBeastAppearance{};
std::atomic_uint64_t context{}; // race ID + independent body/visibility bits
RE::FormID boundRace{};
vampire::Driver requestedDriver{vampire::Driver::automatic};
bool enabled=true,sceneInstalled{},resourcesReady{},resourceFailure{},logSamples{};
std::atomic_bool travelPaused{};
float amplitude{1},flutterCycleSeconds{0.60f};
std::array<vampire::Animation,3> sourceAnimations;
std::array<std::array<std::size_t,10>,3> tracks;
struct Bone {RE::NiPointer<RE::NiAVObject> node;legs::Quaternion before,output;bool wrote{};};
RE::NiPointer<RE::NiAVObject> root;
std::array<Bone,10> bones;
std::array<visual::Clock,3> clocks;
WaveWeights desired{},displayed{};
float peak{},bottom{},extended{},delta{},downEnd{0.32f},wingPitch{};
bool released{};
unsigned samples{};
std::uint64_t requested{},applied{~0ULL};

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
bool current(){auto player=RE::PlayerCharacter::GetSingleton();return player&&player->GetRace()&&player->GetRace()->GetFormID()==boundRace
    &&player->Is3DLoaded()&&!player->IsDead()&&!player->IsInRagdollState()&&!player->IsInKillMove()&&root&&player->Get3D(false)==root.get();}
void restore(){
    if(current())for(auto& b:bones)if(b.node&&b.wrote&&std::abs(legs::dot(rotation(b.node->local.rotate),b.output))>0.999999f)
        rotation(b.node->local.rotate,b.before);
    for(auto& b:bones)b.wrote=false;
}
void drop(){restore();root.reset();boundRace=0;bones={};clocks={};desired=displayed={};released=false;requested=0;applied=~0ULL;samples=0;}
bool load(){
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
    SKSE::log::info("Native Vampire Lord wing geometry: leftPath={} rightPath={} nodes; installed proxy transforms, translations and scales preserved",paths[0].size(),paths[1].size());
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
    SKSE::log::info("Native Vampire Lord wing phases: peak={:.3f} bottom={:.3f} spread={:.3f} heightSpan={:.2f} lateralSpan={:.2f}",peak,bottom,extended,high-low,wide);
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
}
void resolveVampireFlight(const Configuration& c){
    amplitude=c.spreadAmplitudeMultiplier;logSamples=c.logSamples;
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
    std::uint64_t next=0;
    if(a&&a->Is3DLoaded()&&!a->IsDead()&&!a->IsInRagdollState()&&a->GetRace()){
        auto race=a->GetRace();const bool vlrn=vlrnRaceKeyword&&race->HasKeyword(vlrnRaceKeyword);
        const bool known=race==nativeRace||vlrn;
        if(known){const bool nativeBody=graphOf(a);bool injected{};
            const bool humanBody=!nativeBody&&a->GetGraphVariableBool("Wingflight_IsFlying",injected);
            if(vampire::validBody(known,nativeBody,humanBody)){
                bool worn=false;
                // Examine actual worn entries; a selected but unequipped wing does
                // not grant flight. No inventory initialization or script writes.
                if(vlrn&&vlrnSelectedWings&&vlrnNativeWings){
                    if(const auto inv=a->GetInventoryChanges(true);inv&&inv->entryList)
                        for(const auto e:*inv->entryList)if(e&&e->object&&e->IsWorn()
                            &&vlrnSelectedWings->HasForm(e->object)&&vlrnNativeWings->HasForm(e->object)){worn=true;break;}}
                const bool beast=vlrnBeastAppearance&&vlrnBeastAppearance->value==1&&nativeBody;
                const bool rig=vampire::rigVisible(vlrn,beast,worn);
                next=(std::uint64_t(race->GetFormID())<<8)|1|(nativeBody?2:0)|(rig?4:0);
            }
        }
    }
    const auto old=context.exchange(next);
    if(old!=next&&(old||next))SKSE::log::info("Vampire Lord context: race={:08X} body={} skeletalWingsVisible={}; eligibility follows actual transformation/equipment",next>>8,next&2?"VampireLord":next&1?"Character":"none",bool(next&4));
}
bool nativeVampireFlightEligible(const RE::Actor* a,bool ownedFlight){
    if(!enabled||!isNativeVampireLord(a)||!(context.load()&4)||a->IsInKillMove())return false;
    // Bone presence/binding is validated at prepare before any flight starts.
    if(!vampire::eligible(enabled,true,a->Is3DLoaded(),!a->IsDead(),a->IsInRagdollState(),true,true))return false;
    if(!usesVampireLordBehavior(a))return true; // Character graph has no native levitation stance.
    bool levitating{};const bool readable=a->GetGraphVariableBool("isLevitating",levitating);
    static unsigned stanceSamples{};
    if(ownedFlight&&readable&&!levitating&&stanceSamples++<8)
        SKSE::log::info("Native Vampire Lord transient stance: isLevitating=false, retaining owned flight; race/project/actor remain valid");
    if(!readable)diagnostic::report("vampire.levitation-variable-missing","reject native flight provider",a,"isLevitating");
    return vampire::stancePermitsFlight(readable,levitating,ownedFlight);
}
WingPlayback prepareVampireWings(RE::Actor* actor){
    if(!sceneInstalled||!isNativeVampireLord(actor)||actor!=RE::PlayerCharacter::GetSingleton()||actor->IsDead()||actor->IsInRagdollState())return {};
    std::lock_guard guard(stateLock);if(!load())return {};
    auto* object=actor->Get3D(false);if(!object)return {};
    if(root.get()!=object){
        drop();root.reset(object);boundRace=actor->GetRace()->GetFormID();
        for(unsigned i=0;i<10;++i){bones[i].node.reset(object->GetObjectByName(vampire::wingNames[i]));if(!bones[i].node){
            SKSE::log::error("Native Vampire Lord skeleton missing wing: {}; native flight unavailable",vampire::wingNames[i]);drop();return {};}}
        try{choosePhases();}catch(const std::exception& e){SKSE::log::error("Native Vampire Lord phase selection failed: {}",e.what());drop();return {};}
        clocks[0].duration=clocks[1].duration=sourceAnimations[0].duration;
        clocks[2].duration=flutterCycleSeconds;
        clocks[0].rate=0.01f;desired.original=displayed.original=1;
    }
    return playback();
}
void advanceVampireWings(float dt){std::lock_guard guard(stateLock);if(!root)return;if(!current()){drop();return;}
    delta=std::isfinite(dt)?std::clamp(dt,0.0f,0.1f):0;for(auto& c:clocks)c.advance(delta);++requested;
    // Smooth weight commands retain their current value on interrupted fades.
    const float a=1-std::exp(-delta*25);
    displayed.flap+=(desired.flap-displayed.flap)*a;displayed.fast+=(desired.fast-displayed.fast)*a;
    displayed.spread+=(desired.spread-displayed.spread)*a;displayed.original=std::max(0.0f,1-displayed.flap-displayed.fast-displayed.spread);
    if(released&&displayed.original>0.9995f)drop();
}
bool blendVampireWings(float flap,float ownership,float fast){std::lock_guard guard(stateLock);if(!root)return false;
    (void)fast;desired=vampire::nativeWeights(flap,ownership);
    released=ownership<=0;return true;}
void rateVampireWings(float r){std::lock_guard guard(stateLock);if(std::isfinite(r)&&r>=0)clocks[0].rate=r;}
void loopRateVampireWings(float fast,float spread){std::lock_guard guard(stateLock);clocks[1].rate=glidePlaybackScale(displayed.fast,fast);clocks[2].rate=glidePlaybackScale(displayed.spread,spread);}
void pitchVampireWings(float p){std::lock_guard guard(stateLock);wingPitch=std::isfinite(p)?std::clamp(p,0.0f,90.0f):0;}
bool parkVampirePower(){std::lock_guard guard(stateLock);if(!root||displayed.flap>0.0001f)return false;clocks[0].phase=0;clocks[0].rate=0.01f;return true;}
bool parkVampireFast(){std::lock_guard guard(stateLock);if(!root||displayed.fast>0.0001f)return false;clocks[1].phase=0;return true;}
void releaseVampireWings(bool smooth){std::lock_guard guard(stateLock);if(smooth&&current()){desired={0,0,0,1};released=true;}else drop();}
void applyVampireWings(RE::NiAVObject* object){
    std::lock_guard guard(stateLock);if(!root||object!=root.get())return;
    if(!current()){drop();return;}
    if(travelPaused){restore();return;} // Native bat pose/root motion retain ownership.
    if(applied==requested)return;
    applied=requested;
    if(displayed.original>=0.99999f)return;
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
    if(logSamples&&samples++<8)SKSE::log::info("Native Vampire Lord wing-only pose applied: phase={:.3f} flap={:.3f} fast={:.3f} spread={:.3f} original={:.3f}",clocks[0].phase,displayed.flap,displayed.fast,displayed.spread,displayed.original);
}
bool vampireSceneReady(){return sceneInstalled;}
void pauseVampireWingOverlay(bool value){travelPaused=value;}
bool nativeVampireJumpEvent(const char* event){return event&&vampire::jumpRequest(event);}
// The installed scene hook is shared with human overlays; this module never
// replaces an engine function or another plugin's hook independently.
bool installVampireScene(){sceneInstalled=true;return true;}
}
