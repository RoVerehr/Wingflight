#include "actor_flight.hpp"
#include "actor_combat_policy.hpp"
#include "actor_spell_aim_policy.hpp"
#include "vampire_runtime.hpp"
#include "vampire_policy.hpp"
#include "footstep_policy.hpp"
#include "wings.hpp"
#include "gravity_lease.hpp"
#include "flight_pose.hpp"
#include "lower_body.hpp"
#include "runtime_callable.hpp"
#include "continuous_wing_rate.hpp"
#include "flight_pose_policy.hpp"
#include "lower_body_policy.hpp"
#include <SKSE/SKSE.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace wingflight {
namespace {
using aerial::Phase;
const Configuration* settings{};
Wings* providers{}; // One resolver for player, NPCs and all custom provider INIs.
std::atomic_bool installed{},leaderFlying{},suspended{true};
std::atomic_bool anyActors{},anyPoses{};
std::atomic<unsigned> rayBudget{};
std::atomic<unsigned> bindingBudget{};
std::atomic<float> leaderSpeed{};
std::atomic_bool leaderHighSpeed{};
std::atomic<std::uint64_t> simulationTick{};
std::uint32_t frameSerial{}; // Written only by the player frame callback.
std::mutex rosterLock;
float scanAge{},goalAge{};
float formationYaw{};
RE::TESWorldSpace* leaderWorld{};RE::TESObjectCELL* leaderCell{};
RE::NiPoint3 previousLeader{};bool hadLeader{};
RE::TESFaction* followerFaction{};
thread_local RE::Actor* updatingActor{};
thread_local RE::Actor* commandingActor{};
bool flightEvent(RE::Actor* actor,const char* event){
    struct Scope{RE::Actor* previous;~Scope(){commandingActor=previous;}} scope{commandingActor};
    commandingActor=actor;const bool accepted=actor->NotifyAnimationGraph(event);
    if(!accepted)diagnostic::report("npc.locomotion-event-rejected","retain flight movement and native graph",actor,event);
    return accepted;
}
std::array<RE::TESPackage*,10> wwmTravel{};

// Read-only ABI: no registration, party mode or configuration mutation.
struct WayfarerInterface {
    virtual ~WayfarerInterface()=default;
    virtual std::uint32_t GetVersion()const noexcept=0;
    virtual bool RegisterFollower(std::uint32_t,std::int32_t)=0;
    virtual bool UnregisterFollower(std::uint32_t)=0;
    virtual bool ExcludeFollower(std::uint32_t)=0;
    virtual bool IncludeFollower(std::uint32_t)=0;
    virtual bool IsManaged(std::uint32_t)const=0;
    virtual void SetEnabled(bool)=0;
    virtual bool IsEnabled()const=0;
    virtual void SetFormationMode(std::int32_t)=0;
    virtual std::int32_t GetFormationMode()const=0;
    virtual std::uint32_t GetManagedCount()const=0;
};
WayfarerInterface* wayfarer{};
struct PoseBinding {
    RE::ActorHandle owner;
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    std::vector<const RE::hkbCharacter*> characters;
    bool eligible{};int mode{1};
};
std::mutex poseLock;
std::unordered_map<const RE::Actor*,PoseBinding> poseActors;
std::unordered_map<const RE::hkbCharacter*,std::pair<bool,int>> poseCharacters;
std::mutex graphHookLock;
using Notify=bool(*)(RE::IAnimationGraphManagerHolder*,const RE::BSFixedString&);
std::unordered_map<std::uintptr_t,Notify> graphHooks;
using Channels=void(*)(RE::IPostAnimationChannelUpdateFunctor*);
std::mutex channelHookLock;
std::unordered_map<std::uintptr_t,Channels> channelHooks;
void npcChannels(RE::IPostAnimationChannelUpdateFunctor*);
bool graphEvent(RE::IAnimationGraphManagerHolder* holder,const RE::BSFixedString& event){
    const auto table=*reinterpret_cast<const std::uintptr_t*>(holder);Notify previous{};
    {std::lock_guard guard(graphHookLock);if(const auto it=graphHooks.find(table);it!=graphHooks.end())previous=it->second;}
    if(!previous)return false;
    auto actor=static_cast<RE::Actor*>(holder);
    if(commandingActor!=actor&&event.c_str()&&actorFlightActive(actor)&&(aerial::blocksGroundAnimation(true,event.c_str())
        ||aerial::blocksNativeLocomotion(true,commandingActor==actor,event.c_str())))return true;
    return previous(holder,event);
}
void installGraphGate(RE::Actor* actor){
    const auto holder=static_cast<RE::IAnimationGraphManagerHolder*>(actor);
    const auto table=*reinterpret_cast<const std::uintptr_t*>(holder);
    std::lock_guard guard(graphHookLock);if(graphHooks.contains(table))return;
    const auto previous=*reinterpret_cast<std::uintptr_t*>(table+sizeof(void*));
    if(!runtime::executableMemory(previous)){diagnostic::report("npc.graph-event-hook-unavailable","keep native graph events",actor);return;}
    REL::Relocation<std::uintptr_t> vtable{table};graphHooks[table]=reinterpret_cast<Notify>(vtable.write_vfunc(1,graphEvent));
    SKSE::log::info("NPC graph event handoff installed: vtable={:X} chainedTarget={:X}; only owned NPC jump/fall/landing events",table,previous);
}
void installChannels(RE::Actor* actor){
    const auto functor=actor->AsIPostAnimationChannelUpdateFunctor();
    const auto table=*reinterpret_cast<const std::uintptr_t*>(functor);
    std::lock_guard guard(channelHookLock);if(channelHooks.contains(table))return;
    const auto previous=*reinterpret_cast<std::uintptr_t*>(table+sizeof(void*));
    if(!runtime::executableMemory(previous)){diagnostic::report("npc.animation-channels-unavailable","keep native channel update",actor);return;}
    REL::Relocation<std::uintptr_t> vtable{table};channelHooks[table]=reinterpret_cast<Channels>(vtable.write_vfunc(1,npcChannels));
    SKSE::log::info("NPC pre-evaluation animation channels installed: vtable={:X} chainedTarget={:X}",table,previous);
}

struct ActorFlight {
    RE::ActorHandle actor;
    RE::FormID id{};
    RE::NiPointer<RE::bhkCharacterController> controller;
    RE::MovementControllerNPC* movementController{}; // Identity; native Actor owns its lifetime.
    bool restoreAIDriven{};
    RE::NiPointer<RE::NiAVObject> body;
    GravityLease gravity;
    aerial::Motion motion;
    WingRuntime visual;
    VampireWingRuntime nativeWings;
    LogicalWings logical;
    using WingGraphs=std::vector<RE::BSTSmartPointer<RE::BShkbAnimationGraph>>;
    std::atomic<std::shared_ptr<const WingGraphs>> wingGraphs;
    std::atomic<float> continuousRate{};
    std::atomic_bool owned{};
    std::atomic_bool descending{};
    std::mutex stateLock;
    // This lock never spans native calls or the main flight-state lock in an
    // animation callback. The worker reads values, not combat/controller objects.
    std::mutex spellAimLock;
    aerial::SpellAimFrame spellAim;
    std::atomic_uint spellAimFallbacks{};
    unsigned reportedSpellAimFallbacks{};
    const WingProfile* profile{};
    RE::ActiveEffect* effect{}; // Identity only; reacquired from the actor every update.
    Vec3 goal{},normal{},followGoal{},followVelocity{};
    Vec3 targetVelocity{};
    aerial::CombatSession combatSession;
    aerial::CombatSpacing combatSpacing;
    aerial::RangedRetreat rangedRetreat;
    aerial::CombatStyle combatStyle{aerial::CombatStyle::unknown};
    aerial::CombatAction combatAction{aerial::CombatAction::follow};
    aerial::CombatRange combatRange;
    RE::ActorHandle combatTarget;
    RE::FormID combatItem{};
    std::string_view combatEvidenceReason{"not sampled"};
    float combatSampleAge{.1f},combatHeight{};Vec3 combatHold{},combatVelocity{};
    bool combatReady{},combatFleeing{},flightWanted{};
    AutoHover altitude;
    WingMotion strokes;
    float sampleAge{};
    bool following{},blocked{},seen{},bound{},managedIntent{};
    aerial::FollowReason followReason{aerial::FollowReason::noTravel};
    float groundClearance{100},probeAge{},blend{},bindRetry{};
    float groundHeight{},recoveryAge{};bool groundKnown{};
    GroundWingRecovery groundRecovery;
    aerial::LandingBrake landingBrake;
    Vec3 lastPosition{};bool hadPosition{};unsigned landingLogs{};
    unsigned slot{},loggedSampleFailures{};std::uint64_t lastMovementTick{};
    bool clockReported{};
    std::atomic<int> poseMode{1};
    std::atomic_bool lordBody{};
    std::atomic_bool nativeWingAudio{};
    bool restoreGroundStance{};
    std::atomic_bool stancePreparing{};
    RE::NiPointer<RE::NiAVObject> stanceBody;
    vampire::LevitationTakeoff levitation;
    std::atomic<float> graphSpeed{};
    std::atomic<float> graphDirection{};
    std::atomic<RE::IPostAnimationChannelUpdateFunctor*> channelOwner{};
    bool graphSprint{},highSpeed{};
    aerial::Locomotion locomotion;
    aerial::Pursuit pursuit;
    float flapCooldown{};
    WingBlend flapBlend,fastBlend;
    unsigned bodySamples{};
    WingStrokeSound sound;
    WingStrokeSound fastSound;
    WingFlutterSound flutterSound;
    RE::BGSSoundDescriptorForm *powerSound{},*cruiseSound{},*spreadSound{};
    ~ActorFlight(){visual.release(false);if(controller)gravity.release(controller->gravity);}
};
std::unordered_map<RE::FormID,std::shared_ptr<ActorFlight>> actors;
std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<ActorFlight>>>> activeRoster;
using SpellAimTargetProvider=void(*)(void*,const RE::hkbContext&,bool&,RE::hkVector4&,std::uint32_t);
SpellAimTargetProvider originalSpellAimTarget{};
void spellAimTarget(void* provider,const RE::hkbContext& context,bool& hasTarget,RE::hkVector4& point,std::uint32_t userInfo){
    originalSpellAimTarget(provider,context,hasTarget,point,userInfo);
    if(hasTarget||userInfo!=2||!installed.load()||suspended.load()||!anyActors.load()||!context.character)return;
    const auto owner=actorFlightPoseOwner(context.character);
    if(!owner)return;
    const auto list=activeRoster.load();if(!list)return;
    const auto now=static_cast<std::uint32_t>(simulationTick.load()>>32);
    for(const auto& s:*list)if(s->actor.native_handle()==owner.native_handle()){
        aerial::SpellAimFrame aim;
        {std::lock_guard guard(s->spellAimLock);aim=s->spellAim;}
        if(aerial::bridgeSpellAim(hasTarget,userInfo,s->owned.load(),s->descending.load(),suspended.load(),aim,now)){
            point=RE::hkVector4(aim.point.x,aim.point.y,aim.point.z,0);
            hasTarget=true;++s->spellAimFallbacks;
        }
        return;
    }
}
void installSpellAimTarget(){
    if(originalSpellAimTarget)return;
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_BSDirectAtModifierInterface[0]};
    const auto previous=*reinterpret_cast<const std::uintptr_t*>(table.address()+sizeof(void*));
    if(!runtime::executableMemory(previous)){
        SKSE::log::error("NPC spell aim bridge unavailable: native DirectAt target provider is not callable; vtable={:X} target={:X}; flight movement remains available",table.address(),previous);return;
    }
    originalSpellAimTarget=reinterpret_cast<SpellAimTargetProvider>(previous);
    table.write_vfunc(1,spellAimTarget);
    SKSE::log::info("NPC spell aim bridge installed: chainedTarget={:X}; missing magic targets on owned airborne NPCs only; native DirectAt limits and blending retained",previous);
}
struct NativeWingAudio : RE::BSTEventSink<RE::BSAnimationGraphEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event,RE::BSTEventSource<RE::BSAnimationGraphEvent>*)override{
        if(!event||!event->holder||!event->tag.c_str()||directSoundName(event->tag.c_str(),event->payload.c_str()?event->payload.c_str():"")!="NPCVampireLordWing")
            return RE::BSEventNotifyControl::kContinue;
        const auto list=activeRoster.load();if(list)for(const auto& s:*list)
            if(s->id==event->holder->GetFormID()&&s->owned.load()&&s->nativeWingAudio.load())return RE::BSEventNotifyControl::kStop;
        return RE::BSEventNotifyControl::kContinue;
    }
    void bind(RE::Actor* actor){
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(!actor->GetAnimationGraphManager(manager)||!manager)return;
        RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);
        for(const auto& graph:manager->graphs)if(graph&&graph->holder==actor)
            graph->GetEventSource<RE::BSAnimationGraphEvent>()->PrependEventSink(this);
    }
} nativeWingAudio;
void npcChannels(RE::IPostAnimationChannelUpdateFunctor* functor){
    const auto table=*reinterpret_cast<const std::uintptr_t*>(functor);Channels previous{};
    {std::lock_guard guard(channelHookLock);if(const auto it=channelHooks.find(table);it!=channelHooks.end())previous=it->second;}
    if(previous)previous(functor);else return;
    const auto list=activeRoster.load();if(!list)return;
    for(const auto& s:*list)if(s->owned.load()&&!s->descending.load()&&s->channelOwner.load()==functor){
        const auto actor=s->actor.get();if(!actor||actor->AsIPostAnimationChannelUpdateFunctor()!=functor)continue;
        const bool armed=actor->AsActorState()->IsWeaponDrawn()||actor->IsInCombat();
        const bool lord=s->lordBody.load();
        const int mode=s->poseMode.load();const float speed=armed&&!lord?0.f:s->graphSpeed.load();
        if(!actor->SetGraphVariableFloat("Speed",speed))diagnostic::report("npc.animation-channel-missing","keep flight movement and native animation",actor.get(),"Speed");
        actor->SetGraphVariableFloat("SpeedSampled",speed);
        if(lord)actor->SetGraphVariableFloat("Direction",s->graphDirection.load());
        else if(!armed)actor->SetGraphVariableFloat("Direction",0.f);
        actor->SetGraphVariableBool("IsSprinting",(!armed||lord)&&mode==3);
        if(!actor->SetGraphVariableInt("iSyncSprintState",(!armed||lord)&&mode==3?1:0)&&!lord)diagnostic::report("npc.animation-channel-missing","keep flight movement and native animation",actor.get(),"iSyncSprintState");
        return;
    }
}

Vec3 position(RE::Actor* a){const auto p=a->GetPosition();const float scale=RE::bhkWorld::GetWorldScale();return {p.x*scale,p.y*scale,p.z*scale};}
bool safe(RE::Actor* a){return a&&a->Is3DLoaded()&&!a->IsDisabled()&&!a->IsDead()&&!a->IsInRagdollState()&&!a->IsInKillMove()
    &&a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth)>0&&a->GetCharController();}
bool movableNPC(RE::Actor* a){const auto state=a?a->AsActorState():nullptr;return safe(a)&&state&&a->IsAIEnabled()&&!a->IsOnMount()
    &&!state->IsSwimming()&&!state->IsBleedingOut()&&!state->IsUnconscious()
    &&state->GetSitSleepState()==RE::SIT_SLEEP_STATE::kNormal&&state->GetLifeState()!=RE::ACTOR_LIFE_STATE::kRestrained;}
void releaseMovementControl(ActorFlight& s,RE::Actor* a){
    const auto current=a?a->GetActorRuntimeData().movementController.get():nullptr;
    if(current&&current==s.movementController&&s.restoreAIDriven&&current->GetControlsDriven())
        s.movementController->SetAIDriven();
    s.movementController=nullptr;s.restoreAIDriven=false;
}
void acquireMovementControl(ActorFlight& s,RE::Actor* a){
    const auto movement=a->GetActorRuntimeData().movementController.get();
    if(s.movementController!=movement){
        releaseMovementControl(s,a);s.movementController=movement;
        s.restoreAIDriven=movement&&!movement->GetControlsDriven();
    }
    // Use the native ownership transition (including its movement messages),
    // rather than changing AI packages, disabling AI or writing a position.
    if(movement&&!movement->GetControlsDriven()){
        movement->SetControlsDriven();
        SKSE::log::debug("NPC movement ownership acquired: actor={:08X} controller={:X} restoreAI={}; native AI/attacks retained",a->GetFormID(),reinterpret_cast<std::uintptr_t>(movement),s.restoreAIDriven);
    }
}
bool supported(RE::bhkCharacterController* c){return c&&c->surfaceInfo.supportedState==RE::hkpSurfaceInfo::SupportedState::kSupported
    &&c->surfaceInfo.surfaceNormal.quad.m128_f32[2]>.45f;}
bool sameSpace(RE::Actor* a,RE::PlayerCharacter* p){
    auto ac=a->GetParentCell(),pc=p->GetParentCell();if(!ac||!pc)return false;
    return ac->IsInteriorCell()||pc->IsInteriorCell()?ac==pc:a->GetWorldspace()==p->GetWorldspace();
}
void clearFall(RE::bhkCharacterController* c){RE::hkVector4 p;c->GetPositionImpl(p,false);c->fallTime=0;c->fallStartHeight=p.quad.m128_f32[2]*RE::bhkWorld::GetWorldScaleInverse();}
void applyMovement(ActorFlight& s,RE::bhkCharacterController* c,Vec3 v){
    clearFall(c);s.gravity.apply(c->gravity);
    if(s.motion.phase()!=Phase::landing)c->context.currentState=RE::hkpCharacterStateTypes::kInAir;
    c->SetLinearVelocityImpl(RE::hkVector4(v.x,v.y,v.z,0));
}
void publish(RE::Actor* a,bool value,int mode=1,bool humanoid=true){
    a->SetGraphVariableBool("Wingflight_IsFlying",value);a->SetGraphVariableBool("Wingflight_UsePose",value&&humanoid);
    a->SetGraphVariableBool("Wingflight_BodyPoses",value&&humanoid);a->SetGraphVariableInt("Wingflight_BodyPose",humanoid?mode:0);
}
struct FollowObservation {
    aerial::FollowEvidence evidence;
    aerial::FollowDecision decision;
    RE::FormID package{},basePackage{},scene{},target{};
};
FollowObservation follows(RE::Actor* a,RE::PlayerCharacter* p,bool ownedManagedIntent=false,bool ownedFlight=false,bool preparingFlight=false){
    const auto process=a->GetActorRuntimeData().currentProcess;
    RE::NiPointer<RE::TESObjectREFR> target;if(process&&process->followTarget)RE::LookupReferenceByHandle(process->followTarget,target);
    const auto package=a->GetCurrentPackage();
    const auto basePackage=process?process->currentPackage.package:nullptr;
    const auto once=process&&process->middleHigh?process->middleHigh->runOncePackage.package:nullptr;
    const auto scene=a->GetCurrentScene();
    const auto type=package?package->packData.packType.get():RE::PACKAGE_TYPE::kNone;
    const auto procedure=package?package->procedureType.get():RE::PACKAGE_PROCEDURE_TYPE::kNone;
    const bool travel=type==RE::PACKAGE_TYPE::kFollow||type==RE::PACKAGE_TYPE::kAccompany||type==RE::PACKAGE_TYPE::kEscort
        ||procedure==RE::PACKAGE_PROCEDURE_TYPE::kFollowWithEscort||procedure==RE::PACKAGE_PROCEDURE_TYPE::kFollowWithoutEscort;
    const bool member=wayfarer&&wayfarer->IsEnabled()&&wayfarer->IsManaged(a->GetFormID());
    const auto partyMode=member?wayfarer->GetFormationMode():0;
    const bool partyWaiting=member&&(partyMode==4||partyMode==5);
    const bool managed=member&&!partyWaiting&&(ownedManagedIntent||(std::find(wwmTravel.begin(),wwmTravel.end(),package)!=wwmTravel.end()&&package));
    const bool teammate=a->IsPlayerTeammate()||(followerFaction&&a->IsInFaction(followerFaction));
    const auto baseType=basePackage?basePackage->packData.packType.get():RE::PACKAGE_TYPE::kNone;
    const auto baseProcedure=basePackage?basePackage->procedureType.get():RE::PACKAGE_PROCEDURE_TYPE::kNone;
    const bool baseTravel=baseType==RE::PACKAGE_TYPE::kFollow||baseType==RE::PACKAGE_TYPE::kAccompany||baseType==RE::PACKAGE_TYPE::kEscort
        ||baseProcedure==RE::PACKAGE_PROCEDURE_TYPE::kFollowWithEscort||baseProcedure==RE::PACKAGE_PROCEDURE_TYPE::kFollowWithoutEscort;
    FollowObservation result;
    result.evidence={movableNPC(a)&&sameSpace(a,p),partyWaiting||a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer)>0,
        scene!=nullptr,a->IsHostileToActor(p),target.get()==p,teammate&&travel&&(!target||target.get()==p),managed,
        ownedFlight,teammate||member||baseTravel,scene||(once&&once!=basePackage),target&&target.get()!=p};
    result.evidence.inCombat=a->IsInCombat();result.evidence.airCombat=settings->actors.airCombat;
    result.evidence.preparingFlight=preparingFlight;
    if(result.evidence.inCombat&&target){
        const auto combat=a->GetActorRuntimeData().combatController;
        const auto enemy=combat?combat->targetHandle.get():RE::NiPointer<RE::Actor>{};
        // A combat pursuit reference is not a new explicit follow order.
        if(enemy.get()==target.get())result.evidence.otherTarget=false;
    }
    result.decision=aerial::followDecision(result.evidence);
    result.package=package?package->GetFormID():0;result.basePackage=basePackage?basePackage->GetFormID():0;
    result.scene=scene?scene->GetFormID():0;result.target=target?target->GetFormID():0;
    return result;
}
struct NativeCombat {
    RE::NiPointer<RE::Actor> target;
    aerial::CombatStyle style{aerial::CombatStyle::unknown};
    aerial::CombatRange range;
    RE::FormID item{};bool fleeing{};std::string_view reason{"no combat controller"};
};
NativeCombat nativeCombat(RE::Actor* a){
    NativeCombat result;
    const auto controller=a->GetActorRuntimeData().combatController;
    if(!controller)return result;
    if(controller->inactive||controller->stoppedCombat||controller->ignoringCombat){result.reason="native combat inactive or ignored";return result;}
    result.fleeing=controller->state&&controller->state->isFleeing;
    if(result.fleeing){result.reason="native fleeing";return result;}
    if(controller->state&&controller->state->targetLost){result.reason="native target lost";return result;}
    result.target=controller->targetHandle.get();
    if(!result.target)result.target=a->GetActorRuntimeData().currentCombatTarget.get();
    const auto p=RE::PlayerCharacter::GetSingleton();
    if(!result.target||result.target.get()==a||result.target.get()==p||result.target->IsDead()
        ||result.target->IsDisabled()||!result.target->Is3DLoaded()||!sameSpace(result.target.get(),p)
        ||result.target->GetPosition().GetDistance(a->GetPosition())>6000
        ||result.target->GetPosition().GetDistance(p->GetPosition())>5000){result.reason="target invalid, unloaded or outside local combat area";result.target.reset();return result;}
    const auto inventory=controller->inventory;
    if(!inventory||inventory->equippedItems.size()>16){result.reason="native combat equipment unavailable or over limit";return result;}
    result.reason="no selected offensive combat equipment";
    // Observe only native-selected combat equipment. Never enumerate the whole
    // inventory, equip an item, force an attack, or modify a combat controller.
    RE::CombatInventoryItem* selected{};float best=-std::numeric_limits<float>::infinity();
    for(const auto& equipped:inventory->equippedItems){
        const auto item=equipped.item.get();if(!item)continue;
        const auto type=item->GetType();auto style=aerial::CombatStyle::unknown;
        RE::MagicItem* magic{};bool casting=false;
        if(type==RE::CombatInventoryItem::TYPE::kMelee)style=aerial::CombatStyle::melee;
        else if(type==RE::CombatInventoryItem::TYPE::kRanged)style=aerial::CombatStyle::ranged;
        else if(type==RE::CombatInventoryItem::TYPE::kMagic||type==RE::CombatInventoryItem::TYPE::kStaff||type==RE::CombatInventoryItem::TYPE::kScroll){
            magic=static_cast<RE::CombatInventoryItemMagic*>(item)->GetMagic();
            if(!magic||!magic->IsHostile())continue;
            const auto delivery=magic->GetDelivery();
            if(delivery==RE::MagicSystem::Delivery::kTouch)style=aerial::CombatStyle::melee;
            else if(delivery!=RE::MagicSystem::Delivery::kSelf&&delivery!=RE::MagicSystem::Delivery::kNone)style=aerial::CombatStyle::ranged;
            for(const auto hand:{RE::MagicSystem::CastingSource::kLeftHand,RE::MagicSystem::CastingSource::kRightHand}){
                const auto caster=a->GetMagicCaster(hand);
                if(caster&&caster->currentSpell==magic&&(caster->state==RE::MagicCaster::State::kCharging||caster->state==RE::MagicCaster::State::kCasting))casting=true;
            }
        }
        if(style==aerial::CombatStyle::unknown)continue;
        const float score=std::isfinite(item->itemScore)?item->itemScore:0.f;
        // An actual spell cast wins; otherwise retain the engine's equipment score.
        if(!selected||casting||score>best){selected=item;best=casting?std::numeric_limits<float>::infinity():score;result.style=style;}
    }
    if(selected){
        result.reason="native offensive equipment ready";
        const float scale=RE::bhkWorld::GetWorldScale();
        result.range=aerial::combatRange(result.style,selected->GetMinRange()*scale,selected->GetOptimalRange()*scale,selected->GetMaxRange()*scale);
        result.item=selected->item?selected->item->GetFormID():0;
    }
    return result;
}
void refreshSpellAim(ActorFlight& s,RE::Actor* a){
    aerial::SpellAimFrame next;
    // Read the combat system on the actor update thread. Animation workers must
    // not chase live targets, cast objects or an AI controller under stateLock.
    if(s.owned.load()&&!s.descending.load()&&!s.lordBody.load()&&settings->actors.airCombat&&a->IsInCombat()){
        const auto controller=a->GetActorRuntimeData().combatController;
        const auto enemy=s.combatTarget.get();
        if(controller&&enemy&&!controller->inactive&&!controller->stoppedCombat&&!controller->ignoringCombat
            &&(!controller->state||(!controller->state->isFleeing&&!controller->state->targetLost))
            &&controller->targetHandle.native_handle()==s.combatTarget.native_handle()
            &&!enemy->IsDead()&&!enemy->IsDisabled()&&enemy->Is3DLoaded()
            &&sameSpace(enemy.get(),RE::PlayerCharacter::GetSingleton())&&enemy->GetPosition().GetDistance(a->GetPosition())<6000){
            RE::FormID spell{};
            for(const auto hand:{RE::MagicSystem::CastingSource::kRightHand,RE::MagicSystem::CastingSource::kLeftHand}){
                const auto caster=a->GetMagicCaster(hand);
                const auto magic=caster?caster->currentSpell:nullptr;
                if(!magic||!magic->IsHostile()||magic->GetDelivery()!=RE::MagicSystem::Delivery::kAimed
                    ||(caster->state!=RE::MagicCaster::State::kCharging&&caster->state!=RE::MagicCaster::State::kCasting))continue;
                if(caster->desiredTarget&&caster->desiredTarget.native_handle()!=s.combatTarget.native_handle())continue;
                spell=magic->GetFormID();break;
            }
            if(spell){
                std::optional<RE::BSSpinLockGuard> aimGuard;
                // AE 629 inserts this lock inline at 0x68. The dependency's
                // GetAERuntimeData accessor treats it as a pointer; do not use it.
                if(REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_6_629))aimGuard.emplace(REL::RelocateMember<RE::BSSpinLock>(controller,0x68));
                const auto aim=skyrim_cast<RE::CombatProjectileAimController*>(controller->GetRuntimeData().currentAimController);
                if(aim&&aim->combat_control==controller&&!aim->flags.any(RE::CombatAimController::Flags::kDisable)){
                    const auto p=aim->idealAimPosition;
                    const Vec3 point{p.x,p.y,p.z},origin{a->GetPositionX(),a->GetPositionY(),a->GetPositionZ()};
                    if(aerial::usableSpellAimPoint(origin,point))next={point,static_cast<std::uint32_t>(simulationTick.load()>>32),spell,enemy->GetFormID()};
                }
            }
        }
    }
    {std::lock_guard guard(s.spellAimLock);s.spellAim=next;}
}
void combatIntent(ActorFlight& s,RE::Actor* a,float dt,Vec3 at){
    const bool fighting=a->IsInCombat();
    s.combatSampleAge+=dt;
    if(fighting&&settings->actors.airCombat&&s.combatSampleAge>=.1f){
        s.combatSampleAge=0;const auto sample=nativeCombat(a);
        const auto oldTarget=s.combatTarget.get();
        const bool changed=oldTarget.get()!=sample.target.get()||s.combatStyle!=sample.style||s.combatItem!=sample.item;
        if(changed)s.combatHeight=at.z;
        if(changed||s.combatEvidenceReason!=sample.reason){
            SKSE::log::debug("NPC native combat selection: actor={:08X} target={:08X} style={} item={:08X} range=({:.2f},{:.2f},{:.2f}) reason={}; native attacks retained",a->GetFormID(),sample.target?sample.target->GetFormID():0,aerial::combatStyleName(sample.style),sample.item,sample.range.minimum,sample.range.optimal,sample.range.maximum,sample.reason);
        }
        s.combatEvidenceReason=sample.reason;
        s.combatSpacing.observe(sample.target?sample.target->GetFormID():0,sample.style,sample.range);
        s.combatTarget=sample.target?sample.target->GetHandle():RE::ActorHandle{};
        s.combatStyle=sample.style;s.combatRange=sample.range;s.combatItem=sample.item;s.combatFleeing=sample.fleeing;
        s.combatReady=sample.target&&sample.style!=aerial::CombatStyle::unknown;
        s.combatVelocity={};
        if(sample.target)if(const auto physics=sample.target->GetCharController()){
            RE::hkVector4 velocity;physics->GetLinearVelocityImpl(velocity);const auto& v=velocity.quad.m128_f32;
            const Vec3 measured{v[0],v[1],0};if(aerial::valid(measured))s.combatVelocity=aerial::limit(measured,30.f);
        }
    }
    const auto target=s.combatTarget.get();
    const bool validTarget=target&&!target->IsDead()&&!target->IsDisabled()&&target->Is3DLoaded()
        &&sameSpace(target.get(),RE::PlayerCharacter::GetSingleton());
    const auto action=s.combatSession.update(settings->actors.airCombat,fighting,s.combatReady&&validTarget,s.combatFleeing,dt);
    if(action!=s.combatAction){
        s.combatHold=at;
        SKSE::log::debug("NPC combat flight intent: actor={:08X} action={} AirCombat={} ready={} fleeing={}; 0=follow 1=hold 2=engage 3=land",a->GetFormID(),static_cast<int>(action),settings->actors.airCombat,s.combatReady&&validTarget,s.combatFleeing);
        s.combatAction=action;
    }
    const bool airborneCombat=action==aerial::CombatAction::hold||action==aerial::CombatAction::engage;
    s.flightWanted=aerial::combatFlightWanted(settings->actors.enabled,s.following,leaderFlying.load(),s.owned.load(),action);
    s.goal=s.followGoal;s.targetVelocity=s.followVelocity;
    if(airborneCombat){
        s.targetVelocity={};s.goal=s.combatHold;
        if(action==aerial::CombatAction::hold&&validTarget){auto reference=at;reference.z=s.combatHold.z;
            s.goal=s.combatSpacing.holdGoal(target->GetFormID(),reference,position(target.get()),a->GetAngleZ());}
        if(action==aerial::CombatAction::engage){auto reference=at;reference.z=s.combatHeight;
            s.goal=aerial::combatGoal(reference,position(target.get()),s.combatRange,s.combatStyle,a->GetAngleZ());s.targetVelocity=s.combatVelocity;}
    }
    if(!fighting){s.combatSpacing={};s.combatReady=s.combatFleeing=false;s.combatTarget={};s.combatStyle=aerial::CombatStyle::unknown;s.combatItem=0;s.combatSampleAge=.1f;s.combatEvidenceReason="not sampled";}
}
void artEvent(WingMatch match,const char* event){
    if(!match.profile||!match.profile->awuCompatibility||!match.effect||!match.effect->hitEffects)return;
    for(const auto effect:*match.effect->hitEffects)if(effect&&!effect->finished&&effect->GetType()==RE::TEMP_EFFECT_TYPE::kRefModel){
        auto model=static_cast<RE::ModelReferenceEffect*>(effect);
        if(model->GetAttached()&&model->Get3D()&&!model->NotifyAnimationGraph(event))
            diagnostic::report("npc.wing-art-event-rejected","retain provider animation and private wing pose",model,event);
    }
}
bool groundContact(RE::bhkCharacterController* c){
    return c&&aerial::landingContact(supported(c),c->context.currentState==RE::hkpCharacterStateTypes::kOnGround,
        c->surfaceInfo.surfaceNormal.quad.m128_f32[2],c->surfaceInfo.surfaceDistanceExcess);
}
void recoverGroundWings(ActorFlight& s,RE::Actor* a,WingMatch match,float dt){
    s.visual.advance(dt);s.recoveryAge=std::max(0.f,s.recoveryAge-dt);
    if(!match.profile||!match.profile->awuCompatibility||match.profile->continuousFlight||!match.effect||!match.effect->hitEffects)return;
    bool flutter=false;
    const auto table=REL::Relocation<std::uintptr_t>(RE::VTABLE_hkbStateMachine[0]).address();
    for(const auto effect:*match.effect->hitEffects)if(effect&&!effect->finished&&effect->GetType()==RE::TEMP_EFFECT_TYPE::kRefModel){
        auto model=static_cast<RE::ModelReferenceEffect*>(effect);RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(!model->GetAttached()||!model->Get3D()||!model->GetAnimationGraphManager(manager)||!manager)continue;
        RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);
        for(const auto& graph:manager->graphs)if(graph&&graph->projectName=="AnimatedDragonWings"){
            const auto behavior=graph->characterInstance.behaviorGraph;
            if(!behavior||!behavior->activeNodes||behavior->activeNodes->size()>128)continue;
            for(const auto& info:*behavior->activeNodes){const auto node=info.nodeClone;
                if(!node||*reinterpret_cast<const std::uintptr_t*>(node)!=table)continue;
                const auto state=static_cast<const RE::hkbStateMachine*>(node);
                if(state->name.c_str()&&std::string_view(state->name.c_str())=="HandCastingFXBehavior"&&state->isActive&&state->currentStateID==8)flutter=true;
            }
        }
    }
    if(s.groundRecovery.update(dt,groundContact(a->GetCharController()),false,flutter)){
        artEvent(match,"MTState");SKSE::log::debug("NPC grounded wing recovery: actor={:08X}; wing-only fold retry after original AWU opening",a->GetFormID());
    }
}
void releaseLevitation(ActorFlight& s,RE::Actor* a){
    if(s.restoreGroundStance&&safe(a)&&s.stanceBody.get()==a->Get3D(false)&&!a->IsInCombat()){
        const auto stance=inspectVampireLevitation(a);
        if(stance==vampire::LevitationState::levitating){
            const bool accepted=flightEvent(a,"LevitationToggle");
            SKSE::log::debug("NPC Vampire Lord ground stance requested: actor={:08X} accepted={}; native landing transition",a->GetFormID(),accepted);
        }
    }
    s.restoreGroundStance=false;s.stancePreparing=false;s.stanceBody.reset();s.levitation={};
}
bool prepareLevitation(ActorFlight& s,RE::Actor* a,float dt){
    if(!inspectVampireBody(a))return true;
    if(s.stanceBody&&s.stanceBody.get()!=a->Get3D(false))releaseLevitation(s,a);
    const auto stance=inspectVampireLevitation(a);
    const auto decision=s.levitation.update(dt,stance,!a->IsInCombat());
    if(decision.request){
        s.stanceBody.reset(a->Get3D(false));
        const bool accepted=flightEvent(a,"LevitationToggle");
        s.restoreGroundStance=s.restoreGroundStance||accepted;
        SKSE::log::debug("NPC Vampire Lord levitation requested: actor={:08X} accepted={}; waiting for native H2HBehavior levitation state before takeoff",a->GetFormID(),accepted);
    }
    s.stancePreparing=s.restoreGroundStance&&s.levitation.preparingFollow();
    if(decision.timedOut)diagnostic::report("npc.vampire-levitation-timeout","keep native ground movement; retry stance after two seconds",a,"H2HBehavior",static_cast<int>(stance),static_cast<int>(vampire::LevitationState::levitating));
    if(stance==vampire::LevitationState::unavailable)diagnostic::report("npc.vampire-stance-unreadable","wait for native stance before takeoff",a,"H2HBehavior");
    if(decision.ready)SKSE::log::debug("NPC Vampire Lord levitation confirmed: actor={:08X}; native H2HBehavior state=48 settled, takeoff permitted",a->GetFormID());
    return decision.ready;
}
void stop(ActorFlight& s,RE::Actor* a,const char* reason,bool landed=false){
    const bool was=s.motion.active()||s.bound;
    if(was&&safe(a)&&s.body.get()==a->Get3D(false)){
        const auto current=providers->find(a,true);
        if(current.profile==s.profile&&current.effect==s.effect)artEvent(current,"MTState");
    }
    s.owned=false;s.nativeWingAudio=false;s.descending=false;s.motion.reset();s.altitude.reset();s.strokes.reset();s.visual.release(landed);s.nativeWings.release(landed);s.logical.reset();s.continuousRate=0;s.bound=false;s.wingGraphs.store(nullptr);
    {std::lock_guard guard(s.spellAimLock);s.spellAim={};}
    releaseMovementControl(s,a);
    s.combatSession={};s.combatSpacing={};s.rangedRetreat={};s.combatAction=aerial::CombatAction::follow;s.combatTarget={};s.combatStyle=aerial::CombatStyle::unknown;
    s.combatReady=s.combatFleeing=s.flightWanted=false;s.combatSampleAge=.1f;s.combatItem=0;
    s.combatEvidenceReason="not sampled";
    s.groundRecovery.reset();s.recoveryAge=landed?2.2f:0;s.landingBrake={};s.landingLogs=0;s.hadPosition=false;
    releaseActorLowerBody(s.body.get());
    if(a&&s.graphSprint){a->SetGraphVariableInt("iSyncSprintState",0);flightEvent(a,s.lordBody.load()?"sprintStop":"SprintStop");}
    if(a)a->SetGraphVariableBool("IsSprinting",a->AsActorState()->IsSprinting());
    s.locomotion={};s.pursuit={};s.flapCooldown=0;s.flapBlend={};s.fastBlend={};s.bodySamples=0;
    s.graphSprint=s.highSpeed=false;s.poseMode=1;s.graphSpeed=0;s.graphDirection=0;s.channelOwner=nullptr;
    if(s.controller){s.gravity.release(s.controller->gravity);s.controller.reset();}
    if(a){prepareActorFlightPose(a,false);if(safe(a)&&s.body.get()==a->Get3D(false)){
        publish(a,false);if(landed)a->NotifyAnimationGraph("JumpLand");else if(supported(a->GetCharController()))a->NotifyAnimationGraph("JumpLandEnd");}}
    releaseLevitation(s,a);
    s.body.reset();s.effect=nullptr;s.profile=nullptr;s.blend=0;s.sound.reset();s.fastSound.reset();s.flutterSound.reset();
    if(was)SKSE::log::debug("NPC flight ended: actor={:08X} reason={}",a?a->GetFormID():0,reason);
}
void bindContinuous(ActorFlight& s,WingMatch match){
    if(!match.effect||!match.effect->hitEffects)return;
    auto graphs=std::make_shared<ActorFlight::WingGraphs>();
    for(const auto effect:*match.effect->hitEffects)if(effect&&!effect->finished&&effect->GetType()==RE::TEMP_EFFECT_TYPE::kRefModel){
        auto model=static_cast<RE::ModelReferenceEffect*>(effect);RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(!model->GetAttached()||!model->Get3D()||!model->GetAnimationGraphManager(manager)||!manager)continue;
        {RE::BSSpinLockGuard guard(manager->GetRuntimeData().updateLock);for(const auto& g:manager->graphs)if(g)graphs->push_back(g);}
        model->NotifyAnimationGraph("JumpStandingStart");
    }
    s.wingGraphs.store(graphs);
}
bool bind(ActorFlight& s,RE::Actor* a,WingMatch match){
    bool hasVariable{};if(!a->GetGraphVariableBool("Wingflight_IsFlying",hasVariable))return false;
    if(s.recoveryAge>0){s.visual.release(false);s.nativeWings.release(false);s.recoveryAge=0;s.groundRecovery.reset();}
    s.visual.install(settings->spreadAmplitudeMultiplier);
    if(match.profile->continuousFlight){bindContinuous(s,match);const auto graphs=s.wingGraphs.load();if(!graphs||graphs->empty())return false;}
    else if(match.profile->animationless)s.logical.reset(match.profile->timing.glideEntryPhase);
    else if(match.profile->nativeVampire){if(!s.nativeWings.prepare(a).found)return false;}
    else if(!match.profile->nativeVampire){const auto visual=s.visual.prepare(match);if(!visual.found)return false;}
    s.controller.reset(a->GetCharController());s.body.reset(a->Get3D(false));s.profile=match.profile;s.effect=match.effect;s.bound=true;
    s.flapBlend.reset(1);s.fastBlend.reset(0);
    acquireMovementControl(s,a);
    installGraphGate(a);
    installChannels(a);
    s.channelOwner=a->AsIPostAnimationChannelUpdateFunctor();
    s.powerSound=s.cruiseSound=s.spreadSound=nullptr;
    if(match.profile->flapSoundID)s.powerSound=RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSSoundDescriptorForm>(match.profile->flapSoundID,match.profile->plugin);
    if(match.profile->fastSoundID)s.cruiseSound=RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSSoundDescriptorForm>(match.profile->fastSoundID,match.profile->plugin);
    if(match.profile->spreadSoundID)s.spreadSound=RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSSoundDescriptorForm>(match.profile->spreadSoundID,match.profile->plugin);
    s.lordBody=inspectVampireBody(a);
    s.nativeWingAudio=s.lordBody.load()&&match.profile->nativeVampire;
    if(s.nativeWingAudio.load())nativeWingAudio.bind(a);
    publish(a,true,1,!s.lordBody.load());a->NotifyAnimationGraph(s.lordBody.load()?"JumpStandingEnd":"JumpLandEnd");
    if(!match.profile->continuousFlight)artEvent(match,"JumpFall");
    SKSE::log::debug("NPC flight started: actor={:08X} name={} adapter={} followerSlot={} (shared wing resolver; independent NPC movement and visual state)",a->GetFormID(),a->GetName(),match.profile->id,s.slot);
    return true;
}
// Bounded preview rays assist steering, while the original physical controller
// remains authoritative. No world-scale air grid or per-frame actor discovery.
bool ray(RE::bhkCharacterController* c,Vec3 from,Vec3 to,Vec3& normal,float& fraction){
    unsigned remaining=rayBudget.load();while(remaining&&!rayBudget.compare_exchange_weak(remaining,remaining-1)){}
    if(!remaining)return false;
    auto world=c->GetHavokWorld();if(!world)return false;
    RE::bhkPickData pick{};pick.rayInput.from=RE::hkVector4(from.x,from.y,from.z,0);pick.rayInput.to=RE::hkVector4(to.x,to.y,to.z,0);
    pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);pick.rayOutput.Reset();
    {RE::BSReadLockGuard lock(world->worldLock);world->PickObject(pick);}
    if(!pick.rayOutput.HasHit())return false;
    const auto& n=pick.rayOutput.normal.quad.m128_f32;normal={n[0],n[1],n[2]};fraction=pick.rayOutput.hitFraction;return aerial::valid(normal)&&std::isfinite(fraction);
}
void probe(ActorFlight& s,RE::Actor* a,Vec3 at,float dt){
    const bool landing=s.motion.phase()==Phase::landing||!s.flightWanted;
    s.probeAge+=dt;
    if(s.groundKnown)s.groundClearance=std::max(0.f,at.z-s.groundHeight);
    if(s.probeAge<(landing?.075f:.2f)||rayBudget.load()<(landing?1u:4u))return;s.probeAge=0;s.blocked=false;
    // Landing needs only one predictive ground ray, allowing the existing shared
    // 12-ray budget to cover more actors without repeated three-ray steering.
    const float range=landing?aerial::LandingBrake::probeRange(s.motion.velocity().z,settings->tuning.gravity):8.f;
    Vec3 ground;float groundFraction;const auto origin=aerial::add(at,{0,0,.05f});
    s.groundKnown=ray(s.controller.get(),origin,aerial::add(at,{0,0,-range}),ground,groundFraction)&&ground.z>.45f;
    if(s.groundKnown){s.groundHeight=origin.z-(range+.05f)*groundFraction;s.groundClearance=std::max(0.f,at.z-s.groundHeight);}
    else s.groundClearance=100.f;
    if(landing)return;
    const float height=std::clamp(a->GetHeight()*RE::bhkWorld::GetWorldScale(),.5f,4.f);
    const float radius=std::clamp(a->GetBoundRadius()*RE::bhkWorld::GetWorldScale(),.15f,.8f);
    auto toward=aerial::limit(aerial::subtract(s.goal,at),std::max(2.f,aerial::length(s.motion.velocity())*.6f));
    const float horizontal=std::hypot(toward.x,toward.y);
    const Vec3 lateral=horizontal>.01f?Vec3{-toward.y/horizontal,toward.x/horizontal,0}:Vec3{1,0,0};
    for(const float side:{-radius,0.f,radius}){
        auto from=aerial::add(aerial::add(at,aerial::multiply(lateral,side)),{0,0,height*.55f});Vec3 normal;float fraction;
        if(ray(s.controller.get(),from,aerial::add(from,toward),normal,fraction)&&fraction<1){s.blocked=true;s.normal=normal;break;}
    }
}
void update(ActorFlight& s,RE::Actor* a,float dt){
    if(!movableNPC(a)){stop(s,a,"actor unavailable for flight movement");return;}
    const auto match=providers->find(a,s.motion.active());
    if(!match.profile){stop(s,a,"no supported visible wings");return;}
    const auto c=a->GetCharController();
    if(s.bound&&(c!=s.controller.get()||s.body.get()!=a->Get3D(false)||s.effect!=match.effect||s.profile!=match.profile)){
        stop(s,a,"controller/model/provider changed");return;
    }
    if(suspended.load()){
        if(s.motion.active()&&s.controller){clearFall(s.controller.get());s.controller->SetLinearVelocityImpl(RE::hkVector4(0,0,0,0));}
        return;
    }
    const auto frameStep=aerial::consumeFrame(simulationTick.load(),s.lastMovementTick,dt);
    if(!frameStep.advance){
        // Native/third-party updates can overwrite velocity even when no new
        // simulation time elapsed. Retain the existing command without advancing
        // animations, sound markers, acceleration or the takeoff timer twice.
        if(s.motion.active())applyMovement(s,c,s.motion.velocity());
        return;
    }
    if(!s.clockReported){
        s.clockReported=true;
        SKSE::log::debug("NPC flight update clock: actor={:08X} callbackDelta={:.6g} simulationDelta={:.6g} controllerDelta={:.6g} fallback={} revision=game-frame-clock-2",a->GetFormID(),dt,frameStep.dt,c->stepInfo.deltaTime,frameStep.fallback);
    }
    dt=frameStep.dt;
    if(!s.motion.active()&&s.recoveryAge>0){
        if(match.profile->nativeVampire)s.nativeWings.advance(dt);
        recoverGroundWings(s,a,match,dt);
        if(!leaderFlying.load()||!s.following)return;
    }
    const auto at=position(a);
    combatIntent(s,a,dt,at);
    refreshSpellAim(s,a);
    if(s.hadPosition&&s.owned.load()){
        const float moved=aerial::length(aerial::subtract(at,s.lastPosition));
        if(moved>std::max(2.f,aerial::length(s.motion.velocity())*dt*4.f)){
            RE::hkVector4 physical;c->GetPositionImpl(physical,false);const auto& q=physical.quad.m128_f32;
            const auto package=a->GetCurrentPackage();
            SKSE::log::warn("NPC position discontinuity: actor={:08X} phase={} delta={:.3f} dt={:.4f} from=({:.3f},{:.3f},{:.3f}) to=({:.3f},{:.3f},{:.3f}) physics=({:.3f},{:.3f},{:.3f}) package={:08X} controlsDriven={}; Wingflight does not write position",a->GetFormID(),static_cast<int>(s.motion.phase()),moved,dt,s.lastPosition.x,s.lastPosition.y,s.lastPosition.z,at.x,at.y,at.z,q[0],q[1],q[2],package?package->GetFormID():0,a->GetActorRuntimeData().movementController.get()==s.movementController&&s.movementController&&s.movementController->GetControlsDriven());
        }
    }
    s.lastPosition=at;s.hadPosition=true;
    const bool wantsLanding=s.motion.active()&&(!s.flightWanted||s.motion.phase()==Phase::landing);
    const aerial::Frame frame{true,true,s.following,s.flightWanted,wantsLanding?groundContact(c):supported(c),s.blocked,at,s.goal,s.normal,s.groundClearance};
    const auto failure=aerial::sampleFailure(dt,frame);
    if(failure!=aerial::Failure::none){
        const unsigned flag=1u<<static_cast<unsigned>(failure);
        if(!(s.loggedSampleFailures&flag)){
            s.loggedSampleFailures|=flag;
            SKSE::log::warn("NPC movement sample rejected: actor={:08X} reason={} dt={:.6g} position=({:.6g},{:.6g},{:.6g}) target=({:.6g},{:.6g},{:.6g}) supported={} following={} leaderFlying={} (once per actor/reason)",
                a->GetFormID(),aerial::failureReason(failure),dt,at.x,at.y,at.z,s.goal.x,s.goal.y,s.goal.z,frame.supported,frame.following,frame.leaderFlying);
        }
        diagnostic::report(aerial::failureReason(failure),"skip wing binding; release active NPC flight",a,match.profile->id,dt,.15);
        if(s.bound)stop(s,a,"invalid NPC movement sample");
        return;
    }
    s.bindRetry=std::max(0.f,s.bindRetry-dt);
    if(!s.bound){
        if(!frame.following||!frame.leaderFlying){releaseLevitation(s,a);return;}
        // Request the original stance transition while native AI still owns
        // movement. Do not bind wings, lease controls or apply lift until it ends.
        if(!prepareLevitation(s,a,dt))return;
        if(!frame.supported)return;
        if(s.bindRetry>0)return;
        unsigned budget=bindingBudget.load();while(budget&&!bindingBudget.compare_exchange_weak(budget,budget-1)){}
        if(!budget)return;
    }
    // Decide movement from one physical sample BEFORE publishing graph events.
    // In particular, JumpLandEnd in bind() must not invalidate the supporting
    // ground that admitted takeoff earlier in the same update.
    auto nextMotion=s.motion;
    const bool wasLanding=s.motion.phase()==Phase::landing;
    auto movementFrame=frame;
    if(s.bound){probe(s,a,at,dt);movementFrame.blocked=s.blocked;movementFrame.normal=s.normal;movementFrame.groundClearance=s.groundClearance;}
    movementFrame.target=s.goal;
    movementFrame.targetVelocity=s.targetVelocity;
    const bool wasFast=s.highSpeed;
    const bool combat=s.combatAction==aerial::CombatAction::hold||s.combatAction==aerial::CombatAction::engage;
    const auto desired=aerial::subtract(s.goal,at);
    const auto combatEnemy=s.combatTarget.get();
    const bool spacingHeld=s.combatAction==aerial::CombatAction::hold&&combatEnemy&&s.combatSpacing.available(combatEnemy->GetFormID());
    const auto retreat=s.rangedRetreat.update(combat&&combatEnemy?(spacingHeld?aerial::CombatStyle::ranged:s.combatStyle):aerial::CombatStyle::unknown,
        desired,combatEnemy?aerial::subtract(position(combatEnemy.get()),at):Vec3{},spacingHeld?s.combatSpacing.minimum():s.combatRange.minimum,settings->tuning.speed);
    const bool forwardSprint=!combat||(s.combatAction==aerial::CombatAction::engage&&aerial::forwardSprintAllowed(wasFast,desired,s.motion.velocity(),a->GetAngleZ()));
    const bool sprintRequested=frame.following&&frame.leaderFlying&&s.pursuit.update(!combat&&leaderHighSpeed.load(),s.motion.arrived(),aerial::length(desired),settings->actors.followDistance*RE::bhkWorld::GetWorldScale());
    const bool fastPursuit=sprintRequested&&forwardSprint&&!retreat.retreating;
    s.pursuit.advance(dt,settings->tuning);s.flapCooldown=std::max(0.f,s.flapCooldown-dt);
    s.highSpeed=fastPursuit;
    const bool wingDriven=!match.profile->continuousFlight&&!fastPursuit;
    float speedLimit=std::min(30.f,aerial::pursuitSpeedLimit(fastPursuit,settings->tuning,combat?0.f:leaderSpeed.load())*(fastPursuit?s.pursuit.multiplier():1.f));
    if(retreat.retreating)speedLimit=std::min(speedLimit,retreat.limit);
    const auto result=nextMotion.step(dt,movementFrame,
        speedLimit,settings->tuning.acceleration*(fastPursuit?settings->tuning.sprintSpeedMultiplier:1.f),
        wingDriven?&settings->tuning:nullptr,settings->tuning);
    if(result.landed||result.aborted||!nextMotion.active()){
        if(result.aborted){diagnostic::report("npc.takeoff-timeout","release flight and retry after one second",a,match.profile->id);s.bindRetry=1;}
        if(result.landed)SKSE::log::debug("NPC physical touchdown: actor={:08X} clearance={:.3f} known={} support={} nativeState={} normalZ={:.3f} distanceExcess={:.3f}; restore gravity/AI and fade wing ownership",a->GetFormID(),s.groundClearance,s.groundKnown,static_cast<int>(*c->surfaceInfo.supportedState),static_cast<int>(c->context.currentState),c->surfaceInfo.surfaceNormal.quad.m128_f32[2],c->surfaceInfo.surfaceDistanceExcess);
        stop(s,a,result.landed?"safe physical landing":"takeoff timed out without leaving ground",result.landed);return;
    }
    if(!s.bound&&!bind(s,a,match)){s.bindRetry=1;diagnostic::report("npc.wing-binding-unavailable","leave NPC under native AI",a,match.profile->id);return;}
    s.motion=nextMotion;
    if(result.started)SKSE::log::debug("NPC takeoff committed: actor={:08X} dt={:.6g} supported={} velocity=({:.3f},{:.3f},{:.3f}); revision=takeoff-transaction-1",a->GetFormID(),dt,frame.supported,result.velocity.x,result.velocity.y,result.velocity.z);
    s.owned=true;s.stancePreparing=false;
    const bool landing=s.motion.phase()==Phase::landing;s.descending=landing;
    if(landing&&!wasLanding){
        s.strokes.reset();s.altitude.reset();s.landingBrake={};s.pursuit={};s.landingLogs=0;
        prepareActorFlightPose(a,false,0,!s.lordBody.load());releaseActorLowerBody(s.body.get());publish(a,false,0,!s.lordBody.load());s.channelOwner=nullptr;
        if(s.graphSprint){flightEvent(a,s.lordBody.load()?"sprintStop":"SprintStop");s.graphSprint=false;}
        a->SetGraphVariableInt("iSyncSprintState",0);a->SetGraphVariableBool("IsSprinting",false);
        s.locomotion={};flightEvent(a,"JumpFall");artEvent(match,"JumpFall");
        SKSE::log::debug("NPC descent started: actor={:08X} vz={:.3f} reason={}; flight body released, simulated gravity={:.2f}, landing brake follows visible wing stroke",a->GetFormID(),s.motion.velocity().z,s.combatAction==aerial::CombatAction::land?"ground combat / native combat data unavailable":!settings->actors.enabled?"NPC flight disabled":!leaderFlying.load()?"player flight ended":aerial::followReason(s.followReason),settings->tuning.gravity);
    }else if(!landing&&wasLanding){s.strokes.reset();s.landingBrake={};s.channelOwner=a->AsIPostAnimationChannelUpdateFunctor();flightEvent(a,"JumpLandEnd");}
    auto v=result.velocity;const bool fast=fastPursuit;
    const bool fastPose=fast&&(!combat||aerial::forwardSprintAllowed(true,desired,v,a->GetAngleZ()));
    const int mode=fastPose?3:s.motion.arrived()?1:std::hypot(v.x,v.y)>.3f?2:1;
    if(!landing){publish(a,true,mode,!s.lordBody.load());prepareActorFlightPose(a,!s.lordBody.load(),mode);}
    else prepareActorFlightPose(a,false,0,!s.lordBody.load());
    s.poseMode=mode;s.graphSpeed=mode==1?0.f:std::hypot(v.x,v.y)*RE::bhkWorld::GetWorldScaleInverse();
    const float yaw=a->GetAngleZ();
    s.graphDirection=std::hypot(v.x,v.y)>.1f?std::atan2(v.x*std::cos(yaw)-v.y*std::sin(yaw),v.x*std::sin(yaw)+v.y*std::cos(yaw)):0.f;
    const bool nativeBody=!s.lordBody.load()&&(a->AsActorState()->IsWeaponDrawn()||a->IsInCombat());
    const bool sprintGraph=!landing&&mode==3&&!nativeBody;
    const auto change=s.locomotion.update(mode,landing||nativeBody);
    s.graphSprint=sprintGraph;
    a->SetGraphVariableInt("iSyncSprintState",sprintGraph?1:0);a->SetGraphVariableBool("IsSprinting",sprintGraph);
    if(change.sprintStop)flightEvent(a,s.lordBody.load()?"sprintStop":"SprintStop");
    if(change.moveStop&&!s.lordBody.load())flightEvent(a,"moveStop");
    if(change.moveStart&&!s.lordBody.load())flightEvent(a,"moveStart");
    if(change.sprintStart)flightEvent(a,s.lordBody.load()?"sprintStart":"SprintStart");
    if(settings->logSamples&&(change.moveStart||change.moveStop||change.sprintStart||change.sprintStop)&&s.bodySamples++<24)
        SKSE::log::debug("NPC locomotion transition: actor={:08X} mode={} speed={:.2f} moveStart={} moveStop={} sprintStart={} sprintStop={}; native graph/OAR blends",a->GetFormID(),mode,s.graphSpeed.load(),change.moveStart,change.moveStop,change.sprintStart,change.sprintStop);
    s.blend=std::min(1.f,s.blend+dt/std::max(.1f,match.profile->timing.expandSeconds));
    WingPlayback playback;
    if(match.profile->continuousFlight)s.continuousRate=4.f*(fast?settings->bodyTiming.forwardGlideSpeed:1.f);
    else if(match.profile->animationless){
        s.logical.advance(dt);
        s.logical.loopRate(settings->bodyTiming.forwardGlideSpeed,settings->bodyTiming.glideSpeed);
        playback.active=playback.gliding=playback.spreadActive=true;
        playback.phase=s.logical.phase;playback.frequency=s.logical.rate;playback.flapWeight=s.logical.weights.flap;
        playback.glidePhase=s.logical.fastPhase;playback.glideFrequency=s.logical.fastRate;playback.glideWeight=s.logical.weights.fast;
        playback.spreadPhase=s.logical.spreadPhase;playback.spreadFrequency=s.logical.spreadRate;playback.spreadWeight=s.logical.weights.spread;
    }
    else if(match.profile->nativeVampire){
        s.nativeWings.advance(dt);s.nativeWings.loopRate(settings->bodyTiming.forwardGlideSpeed,settings->bodyTiming.glideSpeed);playback=s.nativeWings.prepare(a);
    }else{
        s.visual.advance(dt);
        s.visual.loopRate(settings->bodyTiming.forwardGlideSpeed,settings->bodyTiming.glideSpeed);
        playback=s.visual.prepare(match);
    }
    if(!match.profile->continuousFlight){
        const bool nativeSprintLoop=vampire::largeSprintLoop(match.profile->nativeVampire,!landing,fast);
        const auto timing=(!landing&&(!fast||nativeSprintLoop))?settings->hoverTiming.apply(match.profile->timing):match.profile->timing;
        s.strokes.setHover((wingDriven||nativeSprintLoop)&&!landing);
        if(playback.flapWeight==0&&playback.graphWeight==0&&playback.phase>0.001f){
            const bool parked=match.profile->nativeVampire?s.nativeWings.parkPower():match.profile->animationless?s.logical.parkPower():s.visual.parkPower();
            if(parked){if(match.profile->nativeVampire)playback=s.nativeWings.prepare(a);else if(match.profile->animationless){playback.phase=s.logical.phase;playback.frequency=s.logical.rate;}else playback=s.visual.prepare(match);}
        }
        if(fast&&!wasFast&&settings->tuning.flapOnHighSpeedEntry){
            s.strokes.highSpeedEntryPulse(playback.phase,playback.flapWeight,timing);
            s.flapCooldown=settings->tuning.flapCooldown;s.pursuit.stroke(settings->tuning);
        }else if(fast&&s.pursuit.catchingUp()&&!s.strokes.pending()&&s.flapCooldown<=0&&!landing){
            s.strokes.pulse(true,false);
            s.flapCooldown=settings->tuning.flapCooldown;s.pursuit.stroke(settings->tuning);
        }
        if(landing&&!s.strokes.pending()){
            const auto& t=match.profile->timing;
            const float lead=playback.flapWeight<.5f?t.expandSeconds+.10f:
                playback.phase<t.downstrokeEnd?(1-playback.phase/t.downstrokeEnd)*t.downstrokeSeconds+t.upstrokeSeconds:t.upstrokeSeconds;
            if(s.landingBrake.due(s.motion.velocity().z,settings->tuning.gravity,s.groundClearance,s.groundKnown,lead,t.downstrokeSeconds)){
                s.strokes.highSpeedEntryPulse(playback.phase,playback.flapWeight,t);
                SKSE::log::debug("NPC landing brake requested: actor={:08X} clearance={:.3f} vz={:.3f} raisingLead={:.3f} phase={:.3f}",a->GetFormID(),s.groundClearance,s.motion.velocity().z,lead,playback.phase);
            }
        }
        if(wingDriven&&!landing){
            s.altitude.observe(at.z);s.altitude.descend(s.goal.z);
            if(result.started||(!s.strokes.pending()&&s.altitude.due(s.motion.velocity().z,settings->tuning.gravity,
                s.strokes.automaticLead(timing,playback.phase),timing.downstrokeSeconds,settings->tuning.autoFlapImpulse)))s.strokes.automaticPulse();
        }
        const float recovery=wingDriven?s.altitude.requestDelay(s.motion.velocity().z,settings->tuning.gravity,
            timing.upstrokeSeconds,timing.downstrokeSeconds,settings->tuning.autoFlapImpulse):-1.f;
        if(nativeSprintLoop&&!s.strokes.pending())s.strokes.pulse(false);
        s.strokes.update(dt,{playback.active,playback.gliding,playback.phase,playback.duration,s.motion.velocity().z,
            playback.flapWeight,playback.frequency,recovery,fast,settings->highSpeedFlapHoldSeconds},(wingDriven||nativeSprintLoop)&&!landing,timing);
        if(s.strokes.charge().due)s.strokes.settleCharge(true); // NPC stamina policy remains deferred.
        s.motion.applyWingLift(s.strokes.automaticLiftFraction(),settings->tuning);
        s.motion.applyWingLift(s.strokes.liftFraction(),settings->tuning,false,s.strokes.liftStartsStroke());
        if(landing&&s.strokes.liftFraction()>0){
            if(s.strokes.liftStartsStroke())s.landingBrake.beginStroke(s.motion.velocity().z,settings->tuning.gravity,
                std::max(.01f,1.f-playback.phase/timing.downstrokeEnd+s.strokes.liftFraction()),timing.downstrokeSeconds);
            s.motion.applyLandingBrake(s.landingBrake.impulse(s.strokes.liftFraction()));
        }
        if(match.profile->nativeVampire)s.nativeWings.rate(s.strokes.rate());else if(match.profile->animationless)s.logical.powerRate(s.strokes.rate());else s.visual.powerRate(s.strokes.rate());
        const bool wantsFlap=nativeSprintLoop||s.strokes.wantsFlap();
        s.flapBlend.update(dt,wantsFlap,wantsFlap?match.profile->timing.toFlapSeconds*s.strokes.raiseRatio(match.profile->timing):match.profile->timing.toGlideSeconds);
        s.fastBlend.update(dt,fast,.5f);
        if(match.profile->nativeVampire){s.nativeWings.blend(s.flapBlend.value(),s.blend,s.fastBlend.value());s.nativeWings.pitch(settings->powerStrokeWingPitch*s.fastBlend.value());}
        else if(match.profile->animationless)s.logical.blend(s.flapBlend.value(),s.blend,s.fastBlend.value());
        else{s.visual.blend(s.flapBlend.value(),s.blend,s.fastBlend.value());s.visual.pitch(settings->powerStrokeWingPitch*s.fastBlend.value());}
        v=s.motion.velocity();
    }
    if(landing&&match.profile->continuousFlight
        &&s.landingBrake.due(v.z,settings->tuning.gravity,s.groundClearance,s.groundKnown,0,.34f)){
        s.landingBrake.beginStroke(v.z,settings->tuning.gravity,1,.34f);
        s.motion.applyLandingBrake(s.landingBrake.impulse(dt/.34f));v=s.motion.velocity();
    }
    if(landing&&s.groundKnown&&s.groundClearance<.3f&&v.z<-aerial::LandingBrake::safeSpeed){
        if(s.landingLogs++==0)SKSE::log::warn("NPC landing contact speed guarded: actor={:08X} clearance={:.3f} vz={:.3f}; contact tolerance safety",a->GetFormID(),s.groundClearance,v.z);
        s.motion.limitLandingImpact();v=s.motion.velocity();
    }
    if(!match.profile->continuousFlight){
        auto play=[&](RE::BGSSoundDescriptorForm* descriptor,bool due,float volume=1.f){
            if(!descriptor||!due||volume<=0)return;auto audio=RE::BSAudioManager::GetSingleton();RE::BSSoundHandle handle;
            if(audio&&audio->GetSoundHandle(handle,descriptor)){handle.SetPosition(a->GetPosition());handle.SetObjectToFollow(a->Get3D(false));handle.SetVolume(volume);handle.Play();}
        };
        play(s.powerSound,s.sound.update(playback.active,playback.phase,playback.flapWeight,playback.frequency,match.profile->flapSoundPhase,match.profile->timing.downstrokeEnd));
        play(s.cruiseSound,s.fastSound.update(playback.gliding,playback.glidePhase,playback.glideWeight,playback.glideFrequency,match.profile->fastSoundPhase,1.f));
        play(s.spreadSound,s.flutterSound.update(playback.spreadActive,playback.spreadPhase,playback.spreadWeight,playback.spreadFrequency,
            match.profile->spreadSoundPhase1,match.profile->spreadSoundPhase2),match.profile->spreadSoundVolume*playback.spreadWeight);
    }
    auto facing=v;const auto enemy=s.combatTarget.get();
    if(combat&&enemy)facing=aerial::subtract(position(enemy.get()),at);
    if(((combat&&enemy)||!s.motion.arrived())&&std::hypot(facing.x,facing.y)>.01f&&!a->IsAttacking()&&!a->IsBlocking()){
        auto angle=a->GetAngle();angle.z=approach(angle.z,angle.z+std::remainder(std::atan2(facing.x,facing.y)-angle.z,6.2831853f),6,dt);a->SetAngle(angle);
    }
    applyMovement(s,c,v);
    if(!landing&&!s.lordBody.load())prepareActorLowerBody(a,true,s.lastMovementTick,dt,mode,{{v.x,v.y,v.z},settings->tuning.gravity});
    s.sampleAge+=dt;if(settings->logSamples&&s.sampleAge>=2){s.sampleAge=0;
        SKSE::log::debug("NPC follow sample: actor={:08X} route={} distance={:.2f} position=({:.2f},{:.2f},{:.2f}) goal=({:.2f},{:.2f},{:.2f}) velocity=({:.2f},{:.2f},{:.2f}) blocked={} wingDriven={} liftFraction={:.4f} phase={:.3f}",
            a->GetFormID(),"direct",aerial::length(aerial::subtract(s.goal,at)),at.x,at.y,at.z,s.goal.x,s.goal.y,s.goal.z,v.x,v.y,v.z,s.blocked,wingDriven&&!landing,s.strokes.automaticLiftFraction(),playback.phase);
        SKSE::log::debug("NPC wing timing: actor={:08X} mode={} gravity={:.2f} rate={:.3f} pending={} stroke={} flapWeight={:.3f} automaticImpulse={:.2f} catchUp={} boost={:.3f}",a->GetFormID(),mode,settings->tuning.gravity,playback.frequency,s.strokes.pending(),static_cast<int>(s.strokes.stroke()),playback.flapWeight,settings->tuning.autoFlapImpulse,s.pursuit.catchingUp(),s.pursuit.multiplier());
        if(combat)SKSE::log::debug("NPC combat movement: actor={:08X} style={} retreat={} urgent={} speedLimit={:.2f} sprintRequested={} forwardAllowed={} fast={} desiredForward={:.3f} velocityForward={:.3f} holdingRangedSpacing={}",a->GetFormID(),aerial::combatStyleName(s.combatStyle),retreat.retreating,retreat.urgent,speedLimit,sprintRequested,forwardSprint,fast,aerial::forwardAlignment(desired,a->GetAngleZ()),aerial::forwardAlignment(v,a->GetAngleZ()),spacingHeld);
        const auto fallbacks=s.spellAimFallbacks.load();
        if(fallbacks!=s.reportedSpellAimFallbacks){
            aerial::SpellAimFrame aim;{std::lock_guard guard(s.spellAimLock);aim=s.spellAim;}
            SKSE::log::debug("NPC spell aim bridge: actor={:08X} spell={:08X} target={:08X} point=({:.2f},{:.2f},{:.2f}) fallbackCount={}; native DirectAt pose and smoothing retained",a->GetFormID(),aim.spell,aim.enemy,aim.point.x,aim.point.y,aim.point.z,fallbacks);
            s.reportedSpellAimFallbacks=fallbacks;
        }
    }
}
template<bool Character> struct ActorUpdate {
    static void thunk(RE::Actor* a,float dt){
        // Some hooks call the base Actor update from Character's wrapper.
        // Preserve that chain but run our physical/visual step once per actor.
        if(updatingActor==a){original(a,dt);return;}
        struct Scope {RE::Actor* old;~Scope(){updatingActor=old;}} scope{updatingActor};updatingActor=a;
        std::shared_ptr<ActorFlight> s;
        if(installed.load()&&anyActors.load()&&a&&!a->IsPlayerRef()){
            std::lock_guard guard(rosterLock);if(const auto it=actors.find(a->GetFormID());it!=actors.end())s=it->second;
        }
        // No roster lock crosses native AI/animation update or third-party hooks.
        Vec3 beforeNative{};bool watchPosition=false;
        if(s){std::lock_guard guard(s->stateLock);
            if(s->motion.active()&&safe(a)&&s->controller.get()==a->GetCharController()){
                acquireMovementControl(*s,a);
                beforeNative=position(a);watchPosition=true;
                s->gravity.apply(s->controller->gravity);clearFall(s->controller.get());
                const auto tick=simulationTick.load();const auto delta=tick?std::bit_cast<float>(static_cast<std::uint32_t>(tick)):0.f;
                const auto v=s->motion.velocity();if(!s->descending.load()&&!s->lordBody.load())prepareActorLowerBody(a,true,tick,delta,s->poseMode.load(),{{v.x,v.y,v.z},settings->tuning.gravity});
            }
        }
        original(a,dt);
        if(s){std::lock_guard guard(s->stateLock);
            if(watchPosition&&safe(a)&&s->controller.get()==a->GetCharController()){
                const auto after=position(a);const float moved=aerial::length(aerial::subtract(after,beforeNative));
                const auto tick=simulationTick.load();const float delta=tick?std::bit_cast<float>(static_cast<std::uint32_t>(tick)):0.f;
                if(moved>std::max(2.f,aerial::length(s->motion.velocity())*std::clamp(delta,0.f,.15f)*4.f))
                    SKSE::log::warn("NPC native update position correction: actor={:08X} delta={:.3f} from=({:.3f},{:.3f},{:.3f}) to=({:.3f},{:.3f},{:.3f}) controlsDriven={}; observed across chained Actor::Update",a->GetFormID(),moved,beforeNative.x,beforeNative.y,beforeNative.z,after.x,after.y,after.z,a->GetActorRuntimeData().movementController.get()==s->movementController&&s->movementController&&s->movementController->GetControlsDriven());
            }
            update(*s,a,dt);}
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
void snapshot(){auto list=std::make_shared<std::vector<std::shared_ptr<ActorFlight>>>();for(const auto& [id,s]:actors)list->push_back(s);activeRoster.store(list);anyActors=!actors.empty();}
}

void prepareActorFlightPose(RE::Actor* a,bool active,int mode,bool falling){
    if(!a)return;
    if(!active&&!falling){std::lock_guard guard(poseLock);if(const auto it=poseActors.find(a);it!=poseActors.end()){
        for(const auto c:it->second.characters)poseCharacters.erase(c);poseActors.erase(it);}anyPoses=!poseCharacters.empty();return;}
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    if(!a->GetAnimationGraphManager(manager)||!manager){prepareActorFlightPose(a,false);return;}
    const bool eligible=active&&!a->AsActorState()->IsWeaponDrawn()&&!a->IsInCombat();if(falling)mode=0;
    {
        std::lock_guard guard(poseLock);const auto it=poseActors.find(a);
        if(it!=poseActors.end()&&it->second.manager.get()==manager.get()){
            it->second.eligible=eligible;it->second.mode=mode;for(const auto c:it->second.characters)poseCharacters[c]={eligible,mode};return;}
    }
    PoseBinding next;next.owner=a->GetHandle();next.manager=manager;next.eligible=eligible;next.mode=mode;
    {
        RE::BSSpinLockGuard graphGuard(manager->GetRuntimeData().updateLock);
        for(const auto& g:manager->graphs)if(g&&g->holder==a&&g->projectName.c_str()
            &&pose::graph(legs::canonical(g->projectName.c_str())))next.characters.push_back(&g->characterInstance);
    }
    // Graph callbacks take poseLock; never hold it while acquiring a graph lock.
    std::lock_guard guard(poseLock);
    if(const auto it=poseActors.find(a);it!=poseActors.end())for(const auto c:it->second.characters)poseCharacters.erase(c);
    for(const auto c:next.characters)poseCharacters[c]={eligible,mode};poseActors[a]=std::move(next);anyPoses=!poseCharacters.empty();
}
bool actorFlightPoseSelected(const RE::hkbCharacter* c){
    if(!c||!anyPoses.load())return false;std::lock_guard guard(poseLock);
    const auto it=poseCharacters.find(c);return it!=poseCharacters.end()&&it->second.second>0;
}
bool actorFallClipGuard(const RE::hkbCharacter* c){
    if(!c||!anyPoses.load())return false;std::lock_guard guard(poseLock);
    const auto it=poseCharacters.find(c);return it!=poseCharacters.end()&&it->second.second==0;
}
bool actorFlightPoseEligible(const RE::hkbCharacter* c){
    if(!c||!anyPoses.load())return false;std::lock_guard guard(poseLock);
    const auto it=poseCharacters.find(c);return it!=poseCharacters.end()&&it->second.first;
}
int actorFlightPoseMode(const RE::hkbCharacter* c){
    if(!c||!anyPoses.load())return 0;std::lock_guard guard(poseLock);const auto it=poseCharacters.find(c);return it!=poseCharacters.end()?it->second.second:0;
}
float actorContinuousWingRate(const RE::hkbClipGenerator* clip,const RE::hkbContext& c){
    const auto list=activeRoster.load();if(!list||!c.character||!clip||!clip->name.c_str())return 0;
    if(!continuousWingClip(clip->name.c_str(),clip->mode==RE::hkbClipGenerator::kModeLooping))return 0;
    for(const auto& s:*list){const auto rate=s->continuousRate.load();const auto graphs=s->wingGraphs.load();if(rate>0&&graphs)for(const auto& g:*graphs)if(&g->characterInstance==c.character)return rate;}
    return 0;
}
RE::ActorHandle actorFlightPoseOwner(const RE::hkbCharacter* c){
    if(!c||!anyPoses.load())return {};std::lock_guard guard(poseLock);
    for(const auto& [actor,binding]:poseActors)if(std::find(binding.characters.begin(),binding.characters.end(),c)!=binding.characters.end())return binding.owner;
    return {};
}
bool actorFlightActive(const RE::Actor* a){
    if(!a||a->IsPlayerRef())return false;const auto list=activeRoster.load();if(!list)return false;
    const auto id=a->GetFormID();for(const auto& s:*list)if(s->id==id&&s->owned.load()&&!s->descending.load()){
        const auto owner=s->actor.get();if(owner.get()==a)return true;}
    return false;
}
void resetActorFlight(){
    std::lock_guard guard(rosterLock);suspended=true;leaderFlying=false;simulationTick=0;
    for(auto& [id,s]:actors){std::lock_guard stateGuard(s->stateLock);const auto a=s->actor.get();stop(*s,a.get(),"session reset");}
    actors.clear();snapshot();scanAge=goalAge=0;hadLeader=false;leaderWorld=nullptr;leaderCell=nullptr;
    std::lock_guard poseGuard(poseLock);poseActors.clear();poseCharacters.clear();anyPoses=false;
}
void initializeActorFlight(const Configuration& cfg,Wings& wings){
    settings=&cfg;providers=&wings;
    auto data=RE::TESDataHandler::GetSingleton();followerFaction=data->LookupForm<RE::TESFaction>(0x5c84e,"Skyrim.esm");
    for(unsigned i=0;i<wwmTravel.size();++i)wwmTravel[i]=data->LookupForm<RE::TESPackage>(0x800+i+(i>=5?1:0),"Wayfarer.esp");
    if(const auto module=GetModuleHandleW(L"Wayfarer.dll")){
        const auto get=reinterpret_cast<WayfarerInterface*(*)(std::uint32_t)>(GetProcAddress(module,"Wayfarer_GetInterface"));
        if(get)wayfarer=get(1);
        if(wayfarer&&wayfarer->GetVersion()!=1){wayfarer=nullptr;SKSE::log::warn("Walk With Me interface version unavailable; native follow intent remains enabled");}
    }
    REL::Relocation<std::uintptr_t> actor{RE::VTABLE_Actor[0]},character{RE::VTABLE_Character[0]};
    const auto a=*reinterpret_cast<std::uintptr_t*>(actor.address()+0xAD*8),c=*reinterpret_cast<std::uintptr_t*>(character.address()+0xAD*8);
    if(!runtime::executableMemory(a)||!runtime::executableMemory(c)){SKSE::log::error("NPC flight disabled: Actor/Character Update is not callable");return;}
    ActorUpdate<false>::original=actor.write_vfunc(0xAD,ActorUpdate<false>::thunk);
    ActorUpdate<true>::original=character.write_vfunc(0xAD,ActorUpdate<true>::thunk);
    installSpellAimTarget();
    installed=true;resetActorFlight();
    SKSE::log::info("NPC flight framework 0.4.0: revision=npc-spell-aim-16; Enabled={} AirCombat={} FollowDistance={:.2f} MaxActors={}; native combat selection; native movement ownership leased through takeoff/landing; missing native magic aim targets bridged from projectile solver; per-actor native Vampire Lord wingbeats and body graph; auxiliary/equipment gaps retain flight and same-target ranged spacing; forward-only combat sprint; ranged retreat at half speed or ordinary speed inside minimum range; shared leg overlays; physical touchdown; scan=0.5s combat=0.1s rayBudget=12/frame; WWM={} chainedActor={:X} chainedCharacter={:X}; NPC stamina policy reserved",cfg.actors.enabled,cfg.actors.airCombat,cfg.actors.followDistance,cfg.actors.maxActors,wayfarer!=nullptr,a,c);
}
void updateActorFlightLeader(RE::PlayerCharacter* p,float dt,bool playerFlying,bool paused,float hoverReference,bool highSpeed){
    if(!installed.load()||!settings)return;
    const bool flying=playerFlying&&settings->actors.enabled;leaderHighSpeed=flying&&highSpeed;
    if(!safe(p)){resetActorFlight();return;}
    if(!flying&&!anyActors.load()){
        leaderFlying=false;suspended=paused;scanAge=.5f;goalAge=0;hadLeader=false;return;
    }
    if(!std::isfinite(dt)||dt<=0||dt>.15f){suspended=true;simulationTick=0;return;}
    suspended=paused;leaderFlying=flying;rayBudget=paused?0:12;bindingBudget=paused?0:2;if(paused){simulationTick=0;return;}
    RE::hkVector4 observed;p->GetCharController()->GetLinearVelocityImpl(observed);
    const auto& speed=observed.quad.m128_f32;const float measured=std::sqrt(speed[0]*speed[0]+speed[1]*speed[1]+speed[2]*speed[2]);
    leaderSpeed=std::isfinite(measured)?measured:0;
    const auto cell=p->GetParentCell();const auto point=p->GetPosition();
    const bool changed=hadLeader&&(p->GetWorldspace()!=leaderWorld||((cell&&cell->IsInteriorCell())||(leaderCell&&leaderCell->IsInteriorCell()))&&cell!=leaderCell||point.GetDistance(previousLeader)>4096);
    if(changed)resetActorFlight();suspended=false;leaderFlying=flying;
    if(!hadLeader)formationYaw=p->GetAngleZ();
    if(std::hypot(speed[0],speed[1])>.8f)formationYaw+=std::remainder(std::atan2(speed[0],speed[1])-formationYaw,6.2831853f)*(1-std::exp(-3.f*dt));
    auto leaderGoal=position(p);if(std::isfinite(hoverReference))leaderGoal.z=hoverReference;
    if(++frameSerial==0)++frameSerial;
    simulationTick=aerial::frameTick(frameSerial,dt);
    leaderWorld=p->GetWorldspace();leaderCell=cell;previousLeader=point;hadLeader=true;
    std::lock_guard guard(rosterLock);
    scanAge+=dt;goalAge+=dt;
    if(scanAge>=.5f){
        scanAge=0;for(auto& [id,s]:actors)s->seen=false;
        if(const auto lists=RE::ProcessLists::GetSingleton())lists->ForEachHighActor([&](RE::Actor* a){
            if(a==p||!safe(a)||!sameSpace(a,p)||a->GetPosition().GetDistance(point)>6000)return RE::BSContainer::ForEachResult::kContinue;
            const auto id=a->GetFormID();
            const auto found=actors.find(id);
            const bool previousManaged=found!=actors.end()&&found->second->managedIntent&&found->second->owned.load();
            const bool preparing=flying&&found!=actors.end()&&found->second->stancePreparing.load();
            const auto observation=follows(a,p,previousManaged,found!=actors.end()&&found->second->owned.load(),preparing);
            const bool follow=observation.decision.follow;
            if(found!=actors.end()){std::lock_guard stateGuard(found->second->stateLock);auto& state=*found->second;
                if(state.following!=follow||state.followReason!=observation.decision.reason)
                    SKSE::log::debug("NPC follow intent: actor={:08X} follow={} reason={} package={:08X} basePackage={:08X} scene={:08X} target={:08X} owned={} relationship={} temporaryAction={} waiting={} preparingFlight={}",a->GetFormID(),follow,aerial::followReason(observation.decision.reason),observation.package,observation.basePackage,observation.scene,observation.target,observation.evidence.ownedFlight,observation.evidence.continuingRelationship,observation.evidence.temporaryAction,observation.evidence.waiting,observation.evidence.preparingFlight);
                state.following=follow;state.followReason=observation.decision.reason;state.seen=true;return RE::BSContainer::ForEachResult::kContinue;}
            if(!flying||!follow||!providers->find(a).profile)return RE::BSContainer::ForEachResult::kContinue;
            if(actors.size()>=settings->actors.maxActors){diagnostic::report("npc.flight-capacity","retain native AI for excess actors",a,{},static_cast<double>(actors.size()),settings->actors.maxActors);return RE::BSContainer::ForEachResult::kContinue;}
            auto s=std::make_shared<ActorFlight>();s->actor=a->GetHandle();s->id=id;s->following=s->seen=true;s->followReason=observation.decision.reason;s->slot=static_cast<unsigned>(actors.size());
            const auto package=a->GetCurrentPackage();s->managedIntent=package&&std::find(wwmTravel.begin(),wwmTravel.end(),package)!=wwmTravel.end();
            const float separation=settings->actors.followDistance*RE::bhkWorld::GetWorldScale();
            s->goal=s->followGoal=aerial::add(leaderGoal,aerial::formationOffset(s->slot,formationYaw,separation));
            actors[id]=std::move(s);
            return RE::BSContainer::ForEachResult::kContinue;
        });
        for(auto it=actors.begin();it!=actors.end();){
            auto& s=*it->second;std::lock_guard stateGuard(s.stateLock);
            if(!s.seen){auto a=s.actor.get();stop(s,a.get(),"actor left loaded follow area");it=actors.erase(it);}
            else if(!s.motion.active()&&s.recoveryAge<=0&&(!flying||!s.following)){auto a=s.actor.get();stop(s,a.get(),"follow takeoff cancelled");it=actors.erase(it);}
            else ++it;
        }
        snapshot();
    }
    if(goalAge>=.1f){goalAge=0;
        for(auto& [id,s]:actors){const auto a=s->actor.get();if(!safe(a.get()))continue;std::lock_guard stateGuard(s->stateLock);
            const float separation=settings->actors.followDistance*RE::bhkWorld::GetWorldScale();
            s->followGoal=aerial::add(leaderGoal,aerial::formationOffset(s->slot,formationYaw,separation));
            s->followVelocity={speed[0],speed[1],0};
            if(!s->following||!flying){s->followGoal=position(a.get());s->followVelocity={};}
        }
    }
}
}
