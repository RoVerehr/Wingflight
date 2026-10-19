#include "lower_body.hpp"
#include "actor_state.hpp"
#include "logging.hpp"
#include "lower_body_policy.hpp"
#include "armed_bank.hpp"
#include "flight_pitch.hpp"
#include "config.hpp"
#include "OpenAnimationReplacerAPI-Animations.h"
#include "runtime_support.hpp"
#include "runtime_callable.hpp"
#include "body_callsite.hpp"
#include "vampire_runtime.hpp"
#include "leg_pose_animation.hpp"
#include "flight_pose_policy.hpp"
#include "weapon_leg_handoff.hpp"
#include "actor_flight.hpp"
#include "registered_wings.hpp"
#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace wingflight {
namespace {
constexpr auto bones=legs::poseBoneNames;
std::array<legs::LegPoseAnimation,3> legAnimations;
legs::LegPoseBlend legPoseBlend;
pose::TransitionTiming legPoseTiming;
std::array<float,3> legPoseWeights{1,0,0};
float legClock{};
legs::WeaponLegHandoff weaponLegHandoff;
std::array<std::atomic<const RE::hkbCharacter*>,8> characters{};
std::atomic_bool wanted{},installed{};
std::atomic_bool fastFlight{};
std::atomic_uint64_t frame{};
std::atomic<float> frameDelta{0.016f};
std::atomic<float> bankTurn{};
std::atomic<float> pathPitch{};
std::atomic<float> motionX{},motionY{},motionZ{},motionGravity{4};
legs::LegRelaxation relaxation;
legs::LegRelaxationBlend relaxationBlend;
legs::LegAcceleration legAcceleration;
std::array<legs::DampedLegSwing,2> legSwing;
float relaxationWeight{},relaxationSampleTime{};
float secondaryWeight{1};
bool pitchEnabled{};
float pitchSeconds{0.25f},pitchAngle{};
legs::FlightPitchMotion pitchMotion;
bool bankEnabled{},armedBankEnabled{};
float bankDegrees{20},bankSeconds{0.35f},bankAngle{};
legs::FlightBankMotion bankMotion;
float blendIn{0.3f},blendOut{0.25f},weight{};
bool legEnabled{},leanEnabled{};
bool autoAlignEnabled{};
float alignWeight{};
constexpr std::array<const char*,5> alignBoneNames{
    "NPC Root [Root]","NPC Pelvis [Pelv]","NPC Spine [Spn0]","NPC Spine1 [Spn1]","NPC Spine2 [Spn2]"};
float leanDegrees{60},leanIn{0.75f},leanOut{0.75f},leanWeight{},handoffAge{};
bool leanHandoff{},previousArmed{},previousFast{},comSeen{};
float previousPitch{};
OAR_API::Animations::IAnimationsInterface* oar{};
std::vector<std::string> rules;
struct Clip { std::string identity; bool excepted{}; std::uint64_t seen{}; };
std::mutex clipMutex;
std::mutex poseMutex;
std::unordered_map<const RE::hkbClipGenerator*,Clip> clips; // identity only; never dereference retained pointers
std::unordered_set<std::string> logged;
RE::NiPointer<RE::NiAVObject> root;
struct Bone { RE::NiPointer<RE::NiAVObject> node; legs::Quaternion source{},output{},visible{},handoffSeed{}; bool wrote{},visibleSeen{}; };
struct ActorLegs {
    RE::ActorHandle actor;
    RE::NiPointer<RE::NiAVObject> root;
    RE::NiPointer<RE::BSFlattenedBoneTree> tree;
    std::array<Bone,8> bones;
    Bone center;
    legs::LegPoseBlend poses;
    legs::WeaponLegHandoff handoff;
    legs::LegRelaxationBlend tuning;
    legs::LegAcceleration acceleration;
    std::array<legs::DampedLegSwing,2> swing;
    std::unordered_map<const RE::hkbClipGenerator*,Clip> clips;
    std::uint64_t tick{},applied{~0ULL};
    legs::LegMotionInput motion;
    float dt{},clock{},weight{},relaxed{},secondary{1},lean{};
    int mode{1};bool active{true},complete{};unsigned samples{},retainedSamples{};
};
using ActorLegMap=std::unordered_map<const RE::NiAVObject*,std::shared_ptr<ActorLegs>>;
std::atomic<std::shared_ptr<const ActorLegMap>> actorLegs;
legs::LegRelaxation actorRelaxation;
std::atomic_bool actorSceneReady{};
void applyActorLegs(RE::NiAVObject* object,RE::NiUpdateData* data);
void observeActorLegs(RE::NiAVObject* object);
std::array<Bone,8> cached;
Bone center;
std::array<Bone,5> aligned;
unsigned appliedSamples{};
unsigned retainedSamples{};
unsigned leanRetainedSamples{};
unsigned alignRetainedSamples{};
unsigned bankRetainedSamples{};
std::uint64_t lastFrame{~0ULL};
legs::Quaternion readRotation(const RE::NiMatrix3& r){
    legs::Matrix m{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)m[i][j]=r.entry[i][j];return legs::quaternion(m);
}
void writeRotation(RE::NiMatrix3& r,legs::Quaternion q){
    const auto m=legs::matrix(q);for(int i=0;i<3;++i)for(int j=0;j<3;++j)r.entry[i][j]=m[i][j];
}
void resetLegMotion(){relaxationWeight=relaxationSampleTime=0;secondaryWeight=relaxation.secondaryMotion?1.0f:0.0f;legAcceleration.reset();for(auto& swing:legSwing)swing.reset();legPoseBlend.reset();legPoseWeights={1,0,0};legClock=0;weaponLegHandoff.reset();}
bool parentBasis(RE::NiAVObject* node,RE::NiAVObject* object,RE::NiMatrix3& basis){
    auto ancestor=node?node->parent:nullptr;unsigned depth=0;
    while(ancestor&&ancestor!=object&&depth++<64){basis=ancestor->local.rotate*basis;ancestor=ancestor->parent;}
    if(ancestor!=object)diagnostic::report("body.parent-chain-invalid","skip parent-space motion calculation",node,{},depth,64);
    return ancestor==object;
}
bool safe(RE::PlayerCharacter* p){
    return p&&p->Is3DLoaded()&&!p->IsDead()&&!knockedDown(p)&&!p->IsInKillMove()
        &&p->AsActorState()->GetKnockState()==RE::KNOCK_STATE_ENUM::kNormal;
}
bool excepted(){
    const auto now=frame.load();bool result=false;
    std::lock_guard lock(clipMutex);
    for(auto i=clips.begin();i!=clips.end();){
        if(now-i->second.seen>3)i=clips.erase(i);
        else {result|=i->second.excepted;++i;}
    }
    return result;
}
bool loadPose(){
    const auto resourceRoot=std::filesystem::path("Data/meshes/actors/character");
    const auto paths=pose::resolveDefaults([&](std::string_view relative){
        std::error_code error;return std::filesystem::is_regular_file(resourceRoot/relative,error);
    });
    for(unsigned mode=0;mode<3;++mode){
        try{
            if(paths[mode].empty())throw std::runtime_error("local pose and FAR resource are missing");
            const auto file=resourceRoot/paths[mode];
            const auto size=std::filesystem::file_size(file);
            if(size>8*1024*1024)throw std::runtime_error("animation exceeds 8 MiB limit");
            std::ifstream input(file,std::ios::binary);
            if(!input)throw std::runtime_error("cannot open animation");
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
            if(input.bad())throw std::runtime_error("cannot finish reading animation");
            legAnimations[mode]=legs::LegPoseAnimation::read(bytes);
            SKSE::log::info("Lower-body animation reference: mode={} path={} duration={:.3f}s tracks=8; named leg rotations sampled throughout loop",mode+1,paths[mode],legAnimations[mode].duration);
        }catch(const std::exception& error){
            SKSE::log::error("Lower-body animation overlay disabled: mode={} path={} reason={}; upper-body flight and native animation remain available",mode+1,paths[mode],error.what());return false;
        }
    }
    return true;
}
void loadRules(){
    std::ifstream file("Data/SKSE/Plugins/Wingflight/LowerBodyExceptions.txt");std::string line;unsigned lineNumber{};
    while(std::getline(file,line)){
        ++lineNumber;
        const auto first=line.find_first_not_of(" \t\r");if(first==std::string::npos||line[first]=='#'||line[first]==';')continue;
        line=line.substr(first,line.find_last_not_of(" \t\r")-first+1);
        if(line.size()>2048||rules.size()>=256){SKSE::log::warn("Lower-body exception limit reached; skipped entry; file=LowerBodyExceptions.txt line={} length={} maxLength=2048 loadedRules={} maxRules=256",lineNumber,line.size(),rules.size());continue;}
        rules.push_back(legs::canonical(line));
    }
}
void apply(RE::NiAVObject* object,RE::NiUpdateData* data){
    std::lock_guard poseLock(poseMutex);
    if(!wanted&&weight<=0&&leanWeight<=0&&alignWeight<=0&&std::abs(bankAngle)<0.0001f&&std::abs(pitchAngle)<0.0001f)return;
    const auto player=RE::PlayerCharacter::GetSingleton();
    if(usesVampireLordBehavior(player)){
        weight=leanWeight=alignWeight=bankAngle=pitchAngle=0;root.reset();cached={};center={};aligned={};return;
    }
    if(!safe(player)){
        weight=leanWeight=alignWeight=bankAngle=pitchAngle=0;bankMotion.reset();pitchMotion.reset();resetLegMotion();root.reset();cached={};center={};aligned={};leanHandoff=false;comSeen=false;return;
    }
    if(!object||object!=player->Get3D(false))return;
    if(root.get()!=object){
        root.reset(object);cached={};center={};aligned={};weight=leanWeight=alignWeight=bankAngle=pitchAngle=0;bankMotion.reset();pitchMotion.reset();resetLegMotion();leanHandoff=false;comSeen=false;
        previousArmed=previousFast=false;
        if(legEnabled)for(std::size_t i=0;i<bones.size();++i)cached[i].node.reset(object->GetObjectByName(bones[i]));
        if(legEnabled&&std::any_of(cached.begin(),cached.end(),[](const Bone& b){return !b.node;})){
            for(std::size_t i=0;i<cached.size();++i)if(!cached[i].node)
                diagnostic::report("body.leg-bone-missing","skip leg overlay for model",object,bones[i]);
            SKSE::log::warn("Lower-body overlay: incomplete humanoid leg skeleton, skipping this model");
        }
        if(leanEnabled||bankEnabled||pitchEnabled){center.node.reset(object->GetObjectByName("NPC COM [COM ]"));
            if(!center.node)SKSE::log::warn("Armed sprint lean: COM bone missing, skipping lean for this model");}
        if(autoAlignEnabled)for(std::size_t i=0;i<aligned.size();++i){
            aligned[i].node.reset(object->GetObjectByName(alignBoneNames[i]));
            // This correction owns animation bones below the actor's scene root.
            // Never neutralize a custom skeleton whose Root is the actor node itself.
            if(aligned[i].node.get()==object){
                diagnostic::report("body.alignment-is-scene-root","skip this alignment bone",object,alignBoneNames[i]);aligned[i].node.reset();}
        }
    }
    const bool legsReady=legEnabled&&std::all_of(cached.begin(),cached.end(),[](const Bone& b){return b.node.get()!=nullptr;});
    const auto now=frame.load();if(lastFrame==now)return;lastFrame=now;
    const bool on=wanted.load()&&!excepted();
    const auto ui=RE::UI::GetSingleton();
    const float dt=ui&&ui->GameIsPaused()?0.0f:frameDelta.load();
    relaxation=relaxationBlend.update(dt);
    secondaryWeight=legs::approach(secondaryWeight,relaxation.secondaryMotion,dt,0.25f);
    const bool armed=player->AsActorState()->IsWeaponDrawn(),fast=fastFlight.load();
    const auto weaponState=player->AsActorState()->GetWeaponState();
    auto weaponPhase=legs::WeaponPosePhase::sheathed;
    switch(weaponState){
    case RE::WEAPON_STATE::kWantToDraw:case RE::WEAPON_STATE::kDrawing:weaponPhase=legs::WeaponPosePhase::drawing;break;
    case RE::WEAPON_STATE::kDrawn:weaponPhase=legs::WeaponPosePhase::drawn;break;
    case RE::WEAPON_STATE::kWantToSheathe:case RE::WEAPON_STATE::kSheathing:weaponPhase=legs::WeaponPosePhase::sheathing;break;
    default:break;
    }
    // Cover the native/OAR recovery blend after the weapon state completes.
    // At least the existing 0.75s pose interrupt blend must finish before release.
    const float recoverySeconds=std::max({0.75f,legPoseTiming.movement,blendIn,blendOut,
        fast?std::max(legPoseTiming.sprintIn,legPoseTiming.sprintOut):0.0f});
    const auto handoff=weaponLegHandoff.update(weaponPhase,on&&legsReady,dt,blendIn,recoverySeconds);
    if(handoff.capture&&logging::detailed())
        SKSE::log::debug("Flight leg weapon handoff: state={} armed={} retainFlightLegs=true recovery={:.3f}s; native thigh motion resumes after handoff",static_cast<unsigned>(weaponState),armed,recoverySeconds);
    legs::LegPose target{};
    if(legEnabled){
        if(on&&(armed||handoff.protect))legClock+=dt;
        legPoseWeights=legPoseBlend.update(legs::legPoseMode(fast,motionX.load(),motionY.load()),dt,legPoseTiming);
        std::array<legs::LegPose,3> sampled;
        for(unsigned mode=0;mode<3;++mode)sampled[mode]=legAnimations[mode].sample(legClock);
        target=legs::mixLegPoses(sampled,legPoseWeights);
    }
    pitchAngle=pitchMotion.update(dt,pathPitch.load(),pitchEnabled&&on&&fast,pitchSeconds);
    const bool attacking=player->IsAttacking(),blocking=player->IsBlocking();
    bankAngle=bankMotion.update(dt,bankTurn.load(),legs::flightBankWanted(bankEnabled,on,armed,armedBankEnabled,attacking,blocking),
        bankDegrees,attacking||blocking?0.12f:bankSeconds);
    const bool alignOn=legs::armedIdleAlignWanted(autoAlignEnabled&&leanEnabled,on,armed,fast,attacking,blocking);
    // Idle bias must not continuously cancel authored attack/aim rotations.
    // Release quickly for those actions; ordinary sprint changes use the INI fade.
    const float alignSeconds=alignOn?leanIn:(attacking||blocking?0.12f:leanOut);
    alignWeight=legs::approach(alignWeight,alignOn,dt,alignSeconds);
    const float alignEased=alignWeight*alignWeight*(3-2*alignWeight);
    if(handoff.protect)weight=1; // continuous coverage at both weapon-state edges
    else weight=legs::approach(weight,on&&legsReady&&armed,dt,on&&armed?blendIn:blendOut);
    const float eased=weight*weight*(3-2*weight);
    const bool relaxing=on&&legsReady&&armed&&!handoff.protect&&relaxation.enabled;
    relaxationWeight=legs::approach(relaxationWeight,relaxing,dt,relaxing?blendIn:blendOut);
    const float relaxed=handoff.protect?0.0f:relaxationWeight*relaxationWeight*(3-2*relaxationWeight);
    for(auto& b:aligned)if(b.node){
        auto source=readRotation(b.node->local.rotate);
        if(b.wrote&&std::abs(legs::dot(source,b.output))>0.999999f)source=b.source;
        if(alignEased>0){
            b.source=source;b.output=legs::blend(source,legs::retainLocalBend(source),alignEased);b.wrote=true;
            writeRotation(b.node->local.rotate,b.output);
        }else if(b.wrote){writeRotation(b.node->local.rotate,source);b.wrote=false;}
    }
    if(center.node){
        auto source=readRotation(center.node->local.rotate);
        if(center.wrote&&std::abs(legs::dot(source,center.output))>0.999999f)source=center.source;
        const float nativePitch=legs::forwardPitch(source);
        const bool leanOn=legs::armedSprintLeanWanted(leanEnabled,on,armed,fast,false);
        // Drawing a weapon in an already-leaning sprint should retain that posture,
        // rather than first standing up when the new custom idle activates.
        if(leanOn&&!previousArmed&&comSeen&&previousFast&&std::abs(previousPitch-leanDegrees)<10)leanWeight=1;
        if(!armed&&previousArmed&&previousFast&&fast&&on&&leanWeight>0){leanHandoff=true;handoffAge=0;}
        if(leanHandoff){
            handoffAge+=dt;
            if(!on||armed||!fast||std::abs(nativePitch-leanDegrees)<2
                ||handoffAge>std::min(1.5f,std::max(leanIn,leanOut)+0.1f))leanHandoff=false;
        }
        leanWeight=legs::approach(leanWeight,leanOn||leanHandoff,dt,leanOn?leanIn:leanOut);
        const float leanEased=leanWeight*leanWeight*(3-2*leanWeight);
        if(leanEased>0||alignEased>0||std::abs(bankAngle)>0.0001f||std::abs(pitchAngle)>0.0001f){
            center.source=source;const auto pitched=legs::towardForwardPitch(source,leanDegrees,leanEased);
            center.output=legs::blend(pitched,legs::forwardPitchRotation(legs::forwardPitch(pitched)),alignEased);center.wrote=true;
            previousPitch=legs::forwardPitch(center.output);
            if(std::abs(bankAngle)>0.0001f||std::abs(pitchAngle)>0.0001f) {
                // Roll about the ACTOR's forward axis, expressed in COM-parent
                // coordinates. Use current local transforms after auto-alignment:
                // stale world matrices or a sideways bow Root would pick a wrong axis.
                RE::NiMatrix3 basis;
                auto ancestor=center.node->parent;unsigned depth=0;
                while(ancestor && ancestor!=object && depth++<64){basis=ancestor->local.rotate*basis;ancestor=ancestor->parent;}
                if(ancestor==object){
                    if(std::abs(bankAngle)>0.0001f){
                        const auto axis=basis.Transpose()*RE::NiPoint3(0,1,0);
                        center.output=legs::bankAroundAxis(center.output,bankAngle,axis.x,axis.y,axis.z);
                    }
                    if(std::abs(pitchAngle)>0.0001f){
                        const auto axis=basis.Transpose()*RE::NiPoint3(1,0,0);
                        center.output=legs::pitchAroundAxis(center.output,pitchAngle,axis.x,axis.y,axis.z);
                    }
                }
            }
            writeRotation(center.node->local.rotate,center.output);
        }else{
            if(center.wrote)writeRotation(center.node->local.rotate,source);
            center.wrote=false;center.source=center.output=source;
            previousPitch=legs::forwardPitch(center.output);
        }
        comSeen=true;previousArmed=armed;previousFast=fast;
    }
    // Evaluate leg inertia AFTER COM/Root corrections, using current local
    // transforms rather than world matrices from the previous scene update.
    const auto acceleration=legAcceleration.update(dt,{motionX.load(),motionY.load(),motionZ.load()},relaxing);
    const float yaw=player->GetAngleZ(),sine=std::sin(yaw),cosine=std::cos(yaw);
    const RE::NiPoint3 actorAcceleration(acceleration.x*cosine-acceleration.y*sine,
        acceleration.x*sine+acceleration.y*cosine,acceleration.z);
    const RE::NiPoint3 actorGravity(0,0,-motionGravity.load());
    const legs::Quaternion actorRotation{0,0,std::sin(-yaw*0.5f),std::cos(-yaw*0.5f)};
    if(legsReady)for(std::size_t i=0;i<cached.size();++i){
        auto& b=cached[i];auto source=readRotation(b.node->local.rotate);
        // Never feed our last rendered pose back into an unkeyed animation bone.
        if(b.wrote&&std::abs(legs::dot(source,b.output))>0.999999f)source=b.source;
        auto pose=target[i];float boneWeight=eased;
        if(handoff.capture)b.handoffSeed=b.visibleSeen?b.visible:pose;
        const auto joint=i%4,side=i/4;
        if(joint==0)boneWeight=legs::thighFlightWeight(eased,relaxed,relaxation.thighAnimationWeight);
        else if(joint==1){
            RE::NiMatrix3 basis;
            float drive{};legs::Quaternion parent{};
            if(parentBasis(b.node.get(),object,basis)){
                const auto a=basis.Transpose()*actorAcceleration,g=basis.Transpose()*actorGravity;
                const auto& offset=cached[i+1].node->local.translate;
                drive=legs::kneeDrive(pose,{offset.x,offset.y,offset.z},{a.x,a.y,a.z},{g.x,g.y,g.z});
                parent=legs::multiply(actorRotation,readRotation(basis));
            }
            auto springTuning=relaxation;springTuning.secondaryMotion=secondaryWeight>0;
            const float angle=legSwing[side].update(dt,parent,drive,relaxing&&relaxation.secondaryMotion,springTuning);
            pose=legs::kneeSwing(pose,angle*relaxed*secondaryWeight);
        }else if(joint==2)pose=legs::kneeSwing(pose,-0.25f*legSwing[side].angle()*relaxed*secondaryWeight);
        if(handoff.protect){pose=legs::blend(b.handoffSeed,pose,handoff.poseBlend);boneWeight=1;}
        if(eased>0){
            b.source=source;b.output=legs::blend(source,pose,boneWeight);b.wrote=true;
            writeRotation(b.node->local.rotate,b.output);
        }else if(b.wrote){writeRotation(b.node->local.rotate,source);b.wrote=false;}
        b.visible=readRotation(b.node->local.rotate);b.visibleSeen=true;
    }
    if(relaxationWeight==0){for(auto& swing:legSwing)swing.reset();}
    if(logging::detailed()&&relaxing&&(relaxationSampleTime+=dt)>=2){
        relaxationSampleTime=0;
        const auto difference=[&](unsigned bone){return 114.591559f*std::acos(std::clamp(std::abs(legs::dot(cached[bone].source,target[bone])),0.0f,1.0f));};
        SKSE::log::debug("Armed leg relaxation: authoredThigh={:.2f} weight={:.3f} kneeSwing={:.2f}/{:.2f}deg max={:.2f} stiffness={:.2f} damping={:.2f} poseWeights={:.2f}/{:.2f}/{:.2f} nativeThighVsReference={:.2f}/{:.2f}deg",
            relaxation.thighAnimationWeight,relaxed,legSwing[0].angle(),legSwing[1].angle(),relaxation.maxSwingDegrees,relaxation.stiffness,relaxation.damping,
            legPoseWeights[0],legPoseWeights[1],legPoseWeights[2],difference(0),difference(4));
    }
    if(logging::detailed()&&eased>0&&appliedSamples++<6)
        SKSE::log::debug("Lower-body pose applied: frame={} weight={:.3f} bones=8 updateTime={:.4f}",now,eased,data?data->time:0);
}
struct ThirdPersonUpdate {
    static void thunk(RE::NiAVObject* object,RE::NiUpdateData* data){
        applyActorLegs(object,data);applyVampireWings(object);applyRegisteredWingPoses(object,data);apply(object,data);original(object,data);applyRegisteredWingPoses(object,data,true);observeActorLegs(object);
        // Confirm that the native scene update did not immediately overwrite the
        // eight rotations. This is diagnostic only, never a second pose write.
        if(logging::detailed()&&wanted&&retainedSamples<6){
            std::lock_guard lock(poseMutex);
            if(root.get()==object&&weight>0){
                unsigned retained=0;
                for(const auto& b:cached)if(b.node&&b.wrote&&std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>0.99999f)++retained;
                SKSE::log::debug("Lower-body post-update retained rotations: {}/8",retained);++retainedSamples;
            }
        }
        if(logging::detailed()&&wanted&&leanRetainedSamples<8){
            std::lock_guard lock(poseMutex);
            if(root.get()==object&&center.node&&center.wrote&&leanWeight>0){
                const bool retained=std::abs(legs::dot(readRotation(center.node->local.rotate),center.output))>0.99999f;
                SKSE::log::debug("Armed sprint lean retained={} COMPitch={:.2f} target={:.2f} weight={:.3f}",retained,
                    legs::forwardPitch(center.output),leanDegrees,leanWeight);++leanRetainedSamples;
            }
        }
        if(logging::detailed()&&wanted&&alignRetainedSamples<8){
            std::lock_guard lock(poseMutex);
            if(root.get()==object&&alignWeight>0){
                unsigned retained=0,total=0;
                for(const auto& b:aligned)if(b.node&&b.wrote){++total;
                    if(std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>0.99999f)++retained;}
                SKSE::log::debug("Armed sprint auto-align retained={}/{} weight={:.3f} (actor scene-root yaw unchanged)",retained,total,alignWeight);
                ++alignRetainedSamples;
            }
        }
        if(logging::detailed()&&wanted&&bankRetainedSamples<8){
            std::lock_guard lock(poseMutex);
            if(root.get()==object&&center.node&&center.wrote&&std::abs(bankAngle)>1){
                const bool retained=std::abs(legs::dot(readRotation(center.node->local.rotate),center.output))>0.99999f;
                SKSE::log::debug("Shared flight bank retained={} angle={:.2f} (current actor-forward axis)",retained,bankAngle);
                ++bankRetainedSamples;
            }
        }
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
void refreshActorLegWorld(ActorLegs& s,RE::NiUpdateData& data){
    // Animation evaluation and selected bone passes can bypass NiAVObject::Update.
    // Refresh only changed subtrees; never evaluate controllers/animations twice.
    auto refresh=[&](auto&& self,RE::NiAVObject* node,unsigned depth)->void{
        if(!node||depth>32)return;node->UpdateWorldData(&data);
        if(const auto branch=node->AsNode())for(const auto& child:branch->GetChildren())if(child)self(self,child.get(),depth+1);
    };
    if(s.center.wrote&&s.center.node)refresh(refresh,s.center.node.get(),0);
    else for(unsigned i:{0u,4u})if(s.bones[i].node)refresh(refresh,s.bones[i].node.get(),0);
    if(!s.tree)return;auto& tree=s.tree->GetRuntimeData();
    if(!tree.boneEntries||tree.numBones>2048)return;
    for(unsigned i=0;i<tree.numBones;++i){auto& bone=tree.boneEntries[i];
        if(bone.node){bone.local=bone.node->local;bone.world=bone.node->world;}
        else if(bone.parentIndex>=0&&static_cast<unsigned>(bone.parentIndex)<i)
            bone.world=tree.boneEntries[bone.parentIndex].world*bone.local;
    }
}
template<unsigned Slot,bool Flattened>struct ActorBonePass {
    static void thunk(RE::NiNode* node,RE::NiUpdateData& data,std::uint32_t flags){
        original(node,data,flags);
        applyVampireWings(node,&data,true);
        applyRegisteredWingPoses(node,&data,true);
        const auto list=actorLegs.load();if(!list||list->empty())return;
        auto object=static_cast<RE::NiAVObject*>(node);std::shared_ptr<ActorLegs> state;
        for(unsigned depth=0;object&&depth<16;++depth,object=object->parent){
            const auto it=list->find(object);if(it!=list->end()){state=it->second;break;}
        }
        if(!state)return;
        if constexpr(Flattened){if(state->tree.get()!=node)return;}
        else if(state->root.get()!=node)return;
        applyActorLegs(state->root.get(),&data);
        {std::lock_guard guard(poseMutex);if(state->active&&state->complete&&state->weight>0)refreshActorLegWorld(*state,data);}
        observeActorLegs(state->root.get());
    }
    static inline REL::Relocation<decltype(thunk)> original;
    static bool install(){
        REL::Relocation<std::uintptr_t> table{Flattened?RE::VTABLE_BSFlattenedBoneTree[0]:RE::VTABLE_BSFadeNode[0]};
        const auto previous=*reinterpret_cast<std::uintptr_t*>(table.address()+Slot*8);
        if(!runtime::executableMemory(previous)){SKSE::log::error("NPC bone callback unavailable: flattened={} slot={:X} target={:X}",Flattened,Slot,previous);return false;}
        original=table.write_vfunc(Slot,thunk);
        SKSE::log::info("NPC selective bone callback installed: flattened={} slot={:X} chainedTarget={:X}",Flattened,Slot,previous);return true;
    }
};
void installActorScene(){
    bool ready=ActorBonePass<0x2C,true>::install();ready=ActorBonePass<0x2D,true>::install()&&ready;
    ready=ActorBonePass<0x2E,true>::install()&&ready;
    ready=ActorBonePass<0x2C,false>::install()&&ready;ready=ActorBonePass<0x2D,false>::install()&&ready;
    ready=ActorBonePass<0x2E,false>::install()&&ready;actorSceneReady=ready;
}
void restoreActorLegs(ActorLegs& s){
    for(auto& b:s.bones)if(b.node&&b.wrote&&std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>.999999f)writeRotation(b.node->local.rotate,b.source);
    auto& b=s.center;if(b.node&&b.wrote&&std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>.999999f)writeRotation(b.node->local.rotate,b.source);
}
void applyActorLegs(RE::NiAVObject* object,RE::NiUpdateData*){
    const auto list=actorLegs.load();if(!list||!object)return;
    const auto it=list->find(object);if(it==list->end())return;
    const auto state=it->second;std::lock_guard guard(poseMutex);auto& s=*state;
    const auto actor=s.actor.get();
    if(!s.active||!actor||!actor->Is3DLoaded()||actor->Get3D(false)!=object||actor->IsDead()||knockedDown(actor)||actor->IsInKillMove())return;
    if(s.applied==s.tick){for(auto& b:s.bones)if(b.node&&b.wrote)writeRotation(b.node->local.rotate,b.output);if(s.center.node&&s.center.wrote)writeRotation(s.center.node->local.rotate,s.center.output);return;}
    s.applied=s.tick;const float dt=s.dt;
    bool excluded=false;const auto serial=s.tick>>32;
    for(auto i=s.clips.begin();i!=s.clips.end();)if(serial>i->second.seen+2)i=s.clips.erase(i);else{excluded|=i->second.excepted;++i;}
    const bool on=legEnabled&&s.complete&&!excluded;
    const auto tune=s.tuning.update(dt);const bool armed=actor->AsActorState()->IsWeaponDrawn();
    auto phase=legs::WeaponPosePhase::sheathed;
    switch(actor->AsActorState()->GetWeaponState()){
    case RE::WEAPON_STATE::kWantToDraw:case RE::WEAPON_STATE::kDrawing:phase=legs::WeaponPosePhase::drawing;break;
    case RE::WEAPON_STATE::kDrawn:phase=legs::WeaponPosePhase::drawn;break;
    case RE::WEAPON_STATE::kWantToSheathe:case RE::WEAPON_STATE::kSheathing:phase=legs::WeaponPosePhase::sheathing;break;
    default:break;}
    const auto handoff=s.handoff.update(phase,on,dt,blendIn,std::max({.75f,legPoseTiming.movement,legPoseTiming.sprintIn,legPoseTiming.sprintOut,blendOut}));
    s.weight=handoff.protect?1.f:legs::approach(s.weight,on,dt,on?blendIn:blendOut);
    s.relaxed=legs::approach(s.relaxed,on&&armed&&tune.enabled&&!handoff.protect,dt,on&&armed?blendIn:blendOut);
    s.secondary=legs::approach(s.secondary,tune.secondaryMotion,dt,.25f);
    const float overlayWeight=s.weight*s.weight*(3-2*s.weight),relaxed=s.relaxed*s.relaxed*(3-2*s.relaxed);
    s.clock+=dt;const auto weights=s.poses.update(pose::modeIndex(s.mode),dt,legPoseTiming);
    std::array<legs::LegPose,3> sampled;for(unsigned i=0;i<3;++i)sampled[i]=legAnimations[i].sample(s.clock);
    const auto target=legs::mixLegPoses(sampled,weights);
    if(s.center.node){auto& b=s.center;auto native=readRotation(b.node->local.rotate);if(b.wrote&&std::abs(legs::dot(native,b.output))>.999999f)native=b.source;
        s.lean=legs::approach(s.lean,on&&armed&&s.mode==3&&leanEnabled,dt,s.mode==3?leanIn:leanOut);
        b.source=native;b.output=legs::towardForwardPitch(native,leanDegrees,s.lean*s.lean*(3-2*s.lean));b.wrote=s.lean>0;
        writeRotation(b.node->local.rotate,b.wrote?b.output:native);
    }
    const auto acceleration=s.acceleration.update(dt,s.motion.velocity,on&&armed);
    const float yaw=actor->GetAngleZ(),sine=std::sin(yaw),cosine=std::cos(yaw);
    const RE::NiPoint3 force(acceleration.x*cosine-acceleration.y*sine,acceleration.x*sine+acceleration.y*cosine,acceleration.z);
    const RE::NiPoint3 gravity(0,0,-s.motion.gravity);const legs::Quaternion rotation{0,0,std::sin(-yaw*.5f),std::cos(-yaw*.5f)};
    if(s.complete)for(unsigned i=0;i<8;++i){
        auto& b=s.bones[i];auto native=readRotation(b.node->local.rotate);if(b.wrote&&std::abs(legs::dot(native,b.output))>.999999f)native=b.source;
        auto targetBone=target[i];float blend=overlayWeight;
        if(handoff.capture)b.handoffSeed=b.visibleSeen?b.visible:targetBone;
        const unsigned joint=i%4,side=i/4;
        if(joint==0)blend=legs::thighFlightWeight(overlayWeight,relaxed,tune.thighAnimationWeight);
        else if(joint==1){RE::NiMatrix3 basis;float drive{};legs::Quaternion parent{};
            if(parentBasis(b.node.get(),object,basis)){const auto a=basis.Transpose()*force,g=basis.Transpose()*gravity;const auto offset=s.bones[i+1].node->local.translate;
                drive=legs::kneeDrive(targetBone,{offset.x,offset.y,offset.z},{a.x,a.y,a.z},{g.x,g.y,g.z});parent=legs::multiply(rotation,readRotation(basis));}
            auto spring=tune;spring.secondaryMotion=s.secondary>0;
            const float angle=s.swing[side].update(dt,parent,drive,on&&armed&&tune.secondaryMotion,spring);
            targetBone=legs::kneeSwing(targetBone,angle*relaxed*s.secondary);
        }else if(joint==2)targetBone=legs::kneeSwing(targetBone,-.25f*s.swing[side].angle()*relaxed*s.secondary);
        if(handoff.protect){targetBone=legs::blend(b.handoffSeed,targetBone,handoff.poseBlend);blend=1;}
        b.source=native;b.output=legs::blend(native,targetBone,blend);b.wrote=overlayWeight>0;
        writeRotation(b.node->local.rotate,b.wrote?b.output:native);b.visible=readRotation(b.node->local.rotate);b.visibleSeen=true;
    }
    if(logging::detailed()&&s.samples++<3)SKSE::log::debug("NPC lower-body pose applied: actor={:08X} mode={} armed={} weight={:.3f} weaponHandoff={} relaxed={:.3f}",actor->GetFormID(),s.mode,armed,overlayWeight,handoff.protect,relaxed);
}
void observeActorLegs(RE::NiAVObject* object){
    if(!logging::detailed())return;const auto list=actorLegs.load();if(!list||!object)return;const auto it=list->find(object);if(it==list->end())return;
    std::lock_guard guard(poseMutex);auto& s=*it->second;if(!s.active||!s.complete||s.weight<=0||s.retainedSamples>=3)return;
    unsigned count{};for(const auto& b:s.bones)if(b.node&&b.wrote&&std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>.99999f)++count;
    const auto actor=s.actor.get();SKSE::log::debug("NPC lower-body post-update retained rotations: actor={:08X} {}/8",actor?actor->GetFormID():0,count);++s.retainedSamples;
}
}

void installLowerBody(const Configuration& config){
    actorRelaxation=config.legRelaxation;
    legEnabled=config.lowerBodyEnabled;leanEnabled=config.armedSprintLean;
    relaxation=config.legRelaxation;
    relaxationBlend.initialize(relaxation);secondaryWeight=relaxation.secondaryMotion?1.0f:0.0f;
    legPoseTiming={config.movementBlendSeconds,config.sprintBlendInSeconds,config.sprintBlendOutSeconds};
    pitchEnabled=config.highSpeedPitch;pitchSeconds=config.highSpeedPitchBlendSeconds;
    autoAlignEnabled=config.armedSprintAutoAlign;
    bankEnabled=config.bodyTiming.banking;armedBankEnabled=config.armedBanking;
    bankDegrees=config.bankDegrees;bankSeconds=config.bankBlendSeconds;
    leanDegrees=config.armedSprintLeanDegrees;leanIn=config.armedSprintLeanIn;leanOut=config.armedSprintLeanOut;
    if(!legEnabled&&!leanEnabled&&!bankEnabled&&!pitchEnabled)SKSE::log::info("Human skeleton overlays disabled by INI; native Vampire Lord scene callback remains available");
    blendIn=config.lowerBodyBlendIn;blendOut=config.lowerBodyBlendOut;
    if(legEnabled&&!loadPose())legEnabled=false;
    loadRules();
    const auto module=GetModuleHandleW(L"OpenAnimationReplacer.dll");
    const auto request=module?reinterpret_cast<OAR_API::Animations::_RequestPluginAPI_Animations>(GetProcAddress(module,"RequestPluginAPI_Animations")):nullptr;
    if(request)oar=request(OAR_API::Animations::InterfaceVersion::V1,"Wingflight",REL::Version(0,3,4,0));
    if(!rules.empty()&&!oar){SKSE::log::error("Lower-body overlay disabled: exception rules require OAR Animations API; DLL present={} request export present={} rules={}",
        module!=nullptr,request!=nullptr,rules.size());return;}
    if(!runtime::bodyCallsite(runtimeBackend())) {
        const auto sceneUpdate=REL::RelocationID(68900,70251).address();
        if(!runtimeExecutable(sceneUpdate)) {SKSE::log::error("Body overlay disabled: NiAVObject::Update is not native executable code; address={:X} runtime={}",sceneUpdate,runtimeName());return;}
        const auto init=MH_Initialize();
        if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED) {SKSE::log::error("Body overlay MinHook init failed: {}",static_cast<int>(init));return;}
        decltype(&ThirdPersonUpdate::thunk) native{};
        const auto create=MH_CreateHook(reinterpret_cast<void*>(sceneUpdate),reinterpret_cast<void*>(&ThirdPersonUpdate::thunk),reinterpret_cast<void**>(&native));
        if(create!=MH_OK) {SKSE::log::error("Body overlay hook creation failed: {}",static_cast<int>(create));return;}
        ThirdPersonUpdate::original=reinterpret_cast<std::uintptr_t>(native);
        const auto enabled=MH_EnableHook(reinterpret_cast<void*>(sceneUpdate));
        if(enabled!=MH_OK) {MH_RemoveHook(reinterpret_cast<void*>(sceneUpdate));SKSE::log::error("Body overlay hook enabling failed: {}",static_cast<int>(enabled));return;}
        installed=true;installVampireScene();
        installActorScene();
        SKSE::log::info("Body overlay installed at native scene-update entry for runtime {}; exact player root remains the ownership gate",runtimeName());
        return;
    }
    // Verified third-person call: SE ID39446+0x94; AE1170 ID40522+0x94.
    // Get3D(false) supplies RCX; RDX points to NiUpdateData.
    // Preserve the current call target so Improved Camera / animation replacers
    // remain in the chain. Native destination and argument bytes are observations.
    const auto site=REL::RelocationID(39446,40522).address()+0x94;
    const std::array<std::uint8_t,8> prefix{0xC7,0x44,0x24,0x34,0x01,0x20,0x00,0x00};
    if(!runtimeReadable(reinterpret_cast<void*>(site-8),14)) {
        SKSE::log::error("Lower-body hook failed: third-person call site is unreadable; site={:X} runtime={}",site,runtimeName());return;
    }
    const auto call=runtime::inspectSceneCall(site,{reinterpret_cast<const std::uint8_t*>(site),6});
    if(!call.size) {
        SKSE::log::error("Lower-body hook failed: unsupported call instruction; site={:X} bytes={:02X} {:02X} (supported E8 / FF15)",
            site,*reinterpret_cast<const std::uint8_t*>(site),*reinterpret_cast<const std::uint8_t*>(site+1));return;
    }
    auto destination=call.target;
    if(call.indirect) {
        if(!runtimeReadable(reinterpret_cast<void*>(call.target),sizeof(destination))) {
            SKSE::log::error("Lower-body hook failed: current call pointer slot is unreadable; site={:X} slot={:X}",site,call.target);return;
        }
        std::memcpy(&destination,reinterpret_cast<void*>(call.target),sizeof(destination));
    }
    if(!runtime::executableMemory(destination)||destination==reinterpret_cast<std::uintptr_t>(&ThirdPersonUpdate::thunk)) {
        SKSE::log::error("Lower-body hook failed: current target is not callable or would recurse; site={:X} destination={:X} executable={} selfTarget={}",
            site,destination,runtime::executableMemory(destination),destination==reinterpret_cast<std::uintptr_t>(&ThirdPersonUpdate::thunk));return;
    }
    const auto expected=REL::RelocationID(68900,70251).address();
    const bool prefixMatches=std::memcmp(reinterpret_cast<void*>(site-8),prefix.data(),prefix.size())==0;
    if(!prefixMatches) {
        std::string observedPrefix;
        for(std::size_t i=0;i<prefix.size();++i)observedPrefix+=fmt::format("{:02X} ",*reinterpret_cast<const std::uint8_t*>(site-8+i));
        SKSE::log::warn("Lower-body hook compatibility: argument prefix differs; continuing through current call; site={:X} actualPrefix=[{}] expectedPrefix=[C7 44 24 34 01 20 00 00]",site,observedPrefix);
    }
    SKSE::AllocTrampoline(32);
    // write_call<6> returns the pointer SLOT, not its callable value. Retain the
    // resolved destination before patching, with the exact existing call width.
    ThirdPersonUpdate::original=destination;
    if(call.size==5)SKSE::GetTrampoline().write_call<5>(site,ThirdPersonUpdate::thunk);
    else SKSE::GetTrampoline().write_call<6>(site,ThirdPersonUpdate::thunk);
    installed=true;installVampireScene();
    installActorScene();
    SKSE::log::info("Lower-body hook chain installed: revision=call-chain-1 site={:X} callBytes={} previousDestination={:X} nativeDestination={:X} externalTarget={} argumentPrefixMatches={}",
        site,call.size,destination,expected,destination!=expected,prefixMatches);
    SKSE::log::info("Flight skeleton overlays installed: legs={} armedSprintLean={} autoAlign={} COMPitch={:.2f} blend={:.2f}/{:.2f}s armedBank={} bankMax={:.2f} bankBlend={:.2f}s OAR={} exceptions={}",
        legEnabled,leanEnabled,autoAlignEnabled,leanDegrees,leanIn,leanOut,bankEnabled,bankDegrees,bankSeconds,oar!=nullptr,rules.size());
}
void applyLowerBodySettings(const Configuration& config){
    std::lock_guard lock(poseMutex);
    relaxationBlend.setTarget(config.legRelaxation);
    blendIn=config.lowerBodyBlendIn;blendOut=config.lowerBodyBlendOut;
    actorRelaxation=config.legRelaxation;
    if(const auto list=actorLegs.load())for(const auto& [sceneRoot,s]:*list)s->tuning.setTarget(actorRelaxation);
}
void prepareActorLowerBody(RE::Actor* actor,bool active,std::uint64_t tick,float dt,int mode,legs::LegMotionInput motion){
    if(!actor)return;
    std::lock_guard guard(poseMutex);const auto list=actorLegs.load();const auto object=active?actor->Get3D(false):nullptr;
    if(!active||!actorSceneReady||!legEnabled||!object||usesVampireLordBehavior(actor)){
        if(!list)return;auto next=std::make_shared<ActorLegMap>(*list);bool changed=false;
        for(auto i=next->begin();i!=next->end();)if(i->second->actor.get().get()==actor){auto& s=*i->second;s.active=false;if(actor->Is3DLoaded()&&actor->Get3D(false)==s.root.get())restoreActorLegs(s);i=next->erase(i);changed=true;}else ++i;
        if(changed)actorLegs.store(next);return;
    }
    std::shared_ptr<ActorLegs> state;if(list){const auto it=list->find(object);if(it!=list->end())state=it->second;}
    if(!state){
        auto next=std::make_shared<ActorLegMap>(list?*list:ActorLegMap{});
        if(next->size()>=64){diagnostic::report("npc.lower-body-capacity","keep native legs for excess actor",actor);return;}
        state=std::make_shared<ActorLegs>();state->actor=actor->GetHandle();state->root.reset(object);state->tuning.initialize(actorRelaxation);
        for(unsigned i=0;i<8;++i){state->bones[i].node.reset(object->GetObjectByName(bones[i]));if(!state->bones[i].node)diagnostic::report("npc.leg-bone-missing","skip NPC leg overlay",actor,bones[i]);}
        state->complete=std::all_of(state->bones.begin(),state->bones.end(),[](const auto& b){return b.node.get()!=nullptr;});
        if(state->bones[0].node)for(auto node=state->bones[0].node->parent;node;node=node->parent)
            if(const auto tree=netimmerse_cast<RE::BSFlattenedBoneTree*>(node)){state->tree.reset(tree);break;}
        state->center.node.reset(object->GetObjectByName("NPC COM [COM ]"));(*next)[object]=state;actorLegs.store(next);
    }
    state->tick=tick;state->dt=std::isfinite(dt)?std::clamp(dt,0.f,.1f):0;state->mode=mode;state->motion=motion;
}
void releaseActorLowerBody(RE::NiAVObject* object){
    if(!object)return;std::lock_guard guard(poseMutex);const auto list=actorLegs.load();if(!list)return;
    const auto found=list->find(object);if(found==list->end())return;auto& s=*found->second;s.active=false;
    const auto actor=s.actor.get();if(actor&&actor->Is3DLoaded()&&actor->Get3D(false)==object)restoreActorLegs(s);
    auto next=std::make_shared<ActorLegMap>(*list);next->erase(object);actorLegs.store(next);
}
void prepareLowerBody(RE::PlayerCharacter* player,bool fullFlight,float dt,bool highSpeed,float turn,float flightPitch,legs::LegMotionInput motion){
    if(!installed)return;
    frame.fetch_add(1);frameDelta=std::isfinite(dt)?std::clamp(dt,0.0f,0.1f):0;
    const bool active=fullFlight&&safe(player)&&!usesVampireLordBehavior(player);wanted=active;fastFlight=active&&highSpeed;
    bankTurn=active&&std::isfinite(turn)?turn:0;
    pathPitch=active&&std::isfinite(flightPitch)?flightPitch:0;
    motionX=active&&std::isfinite(motion.velocity.x)?motion.velocity.x:0;
    motionY=active&&std::isfinite(motion.velocity.y)?motion.velocity.y:0;
    motionZ=active&&std::isfinite(motion.velocity.z)?motion.velocity.z:0;
    motionGravity=std::isfinite(motion.gravity)?std::clamp(motion.gravity,0.1f,20.0f):4;
    if(!active){for(auto& c:characters)c=nullptr;std::lock_guard lock(clipMutex);clips.clear();return;}
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if(!player->GetAnimationGraphManager(manager)||!manager){
        diagnostic::report("body.animation-graph-missing","disable overlay for current frame",player);
        wanted=false;for(auto& c:characters)c=nullptr;return;}
    RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);std::size_t i=0;
    for(const auto& graph:manager->graphs)if(graph&&graph->holder==player&&i<characters.size())characters[i++]=&graph->characterInstance;
    while(i<characters.size())characters[i++]=nullptr;
}
void resetLowerBody(){
    wanted=false;fastFlight=false;bankTurn=0;pathPitch=0;for(auto& c:characters)c=nullptr;
    {std::lock_guard lock(clipMutex);clips.clear();}
    std::lock_guard lock(poseMutex);
    if(const auto list=actorLegs.load())for(const auto& [object,s]:*list){s->active=false;const auto actor=s->actor.get();if(actor&&actor->Is3DLoaded()&&actor->Get3D(false)==object)restoreActorLegs(*s);}
    actorLegs.store(nullptr);
    const auto player=RE::PlayerCharacter::GetSingleton();
    if(safe(player)&&root.get()==player->Get3D(false)&&center.node&&center.wrote
        &&std::abs(legs::dot(readRotation(center.node->local.rotate),center.output))>0.999999f)
        writeRotation(center.node->local.rotate,center.source);
    if(safe(player)&&root.get()==player->Get3D(false))for(const auto& b:aligned)
        if(b.node&&b.wrote&&std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>0.999999f)
            writeRotation(b.node->local.rotate,b.source);
    if(safe(player)&&root.get()==player->Get3D(false))for(const auto& b:cached)
        if(b.node&&b.wrote&&std::abs(legs::dot(readRotation(b.node->local.rotate),b.output))>0.999999f)
            writeRotation(b.node->local.rotate,b.source);
    root.reset();cached={};center={};aligned={};weight=leanWeight=alignWeight=bankAngle=pitchAngle=0;bankMotion.reset();pitchMotion.reset();resetLegMotion();leanHandoff=false;comSeen=false;lastFrame=~0ULL;
}
float armedSprintLeanAngle(){std::lock_guard lock(poseMutex);return center.wrote?legs::forwardPitch(center.output):0;}
float flightBankAngle(){std::lock_guard lock(poseMutex);return bankAngle;}
float flightPitchAngle(){std::lock_guard lock(poseMutex);return pitchAngle;}
void observeLowerBodyClip(RE::hkbClipGenerator* clip,const RE::hkbContext& context){
    if(!clip||!context.character||(!logging::detailed()&&rules.empty()))return;
    bool player=false;if(wanted)for(auto& c:characters)if(c.load()==context.character){player=true;break;}
    RE::NiPointer<RE::Actor> npc;
    if(!player){if(!actorLegs.load())return;npc=actorFlightPoseOwner(context.character).get();if(!npc)return;}
    std::string identity;
    if(oar){const auto info=oar->GetCurrentReplacementAnimationInfo(clip);if(info.animationPath.c_str())identity=info.animationPath.c_str();
        if(!identity.empty()&&info.variantFilename.c_str()&&info.variantFilename.c_str()[0])identity+="|variant="+std::string(info.variantFilename.c_str());}
    if(identity.empty()&&clip->animationName.c_str())identity=clip->animationName.c_str();
    identity=legs::canonical(identity);if(identity.empty())return;
    const bool excluded=std::any_of(rules.begin(),rules.end(),[&](const auto& r){return legs::matches(r,identity);});
    if(npc){std::lock_guard lock(poseMutex);const auto list=actorLegs.load();if(!list)return;
        const auto it=list->find(npc->Get3D(false));if(it==list->end())return;auto& s=*it->second;
        if(excluded&&(s.clips.contains(clip)||s.clips.size()<256))s.clips[clip]={identity,true,s.tick>>32};else s.clips.erase(clip);return;
    }
    std::lock_guard lock(clipMutex);
    // Track only exceptions. Ordinary helper/locomotion clips must not fill a
    // bounded table and prevent the actual exceptional attack from being seen.
    if(excluded)clips[clip]={identity,true,frame.load()};else clips.erase(clip);
    if(logging::detailed()&&logged.size()<256&&logged.insert(identity).second)
        SKSE::log::debug("LowerBody clip: {} | keepOriginal={}",identity,excluded);
}
void forgetLowerBodyClip(RE::hkbClipGenerator* clip){
    {std::lock_guard lock(clipMutex);clips.erase(clip);}
    if(!actorLegs.load())return;std::lock_guard lock(poseMutex);if(const auto list=actorLegs.load())for(const auto& [sceneRoot,s]:*list)s->clips.erase(clip);
}
}
