#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <array>
#include <atomic>
#include <mutex>
#include "config.hpp"
#include "flight_save.hpp"
#include "wings.hpp"
#include "continuous_wing_rate.hpp"
#include "vampire_runtime.hpp"
#include "vampire_policy.hpp"
#include "vampire_travel.hpp"
#include "wing_blend.hpp"
#include "tdm_yaw.hpp"
#include "air_input.hpp"
#include "glide_clip.hpp"
#include "awu_compatibility.hpp"
#include "lower_body.hpp"
#include "flight_pitch.hpp"
#include "footstep_policy.hpp"
#include "flight_audio.hpp"
#include "armed_idle_policy.hpp"
#include "armed_sprint_clip.hpp"
#include "flight_pose.hpp"
#include "flight_pose_policy.hpp"
#include "settings_menu.hpp"
#include "settings_file.hpp"
#include "live_settings.hpp"
#include "runtime_support.hpp"
#include "gravity_lease.hpp"
#include "airborne_exit.hpp"
#include "gamepad_input.hpp"
#include "menu_flight_pause.hpp"
#include "diagnostics.hpp"
#include "flight_collision.hpp"
#include "held_wings.hpp"
#include "jump_wing_preview.hpp"

namespace {
using namespace wingflight;
Configuration config;
Flight flight;
FlightCollision flightCollision;
MenuFlightPause menuFlightPause;
vampire::BatTravel nativeBatTravel;
bool nativeTravelWasPaused{};
unsigned nativeBatVelocitySamples{};
std::atomic_bool tweenHandoffPending{},flightMenuSuspended{};
struct OwnedGravity {
    RE::NiPointer<RE::bhkCharacterController> controller;
    GravityLease lease;
    void release(){if(controller){lease.release(controller->gravity);controller.reset();}}
    void apply(RE::bhkCharacterController* current){
        if(current!=controller.get()){release();controller.reset(current);}
        if(controller)lease.apply(controller->gravity);
    }
} ownedGravity;
Wings wings;
std::atomic_bool ready{};
bool loaded{}, resetGraphs{true};
int resumeMode{-1};
Mode preferredFlightMode{Mode::free};
float resumeWait{}, sampleTimer{}, wingRetry{}, ceilingTimer{}, bodyRetry{};
std::atomic_bool managedAirAnimation{};
std::atomic_uint blockedBodyEvents{};
std::atomic_uint blockedSprintEvents{};
ArmedSprintLatch armedSprintLatch;
WingMotion wingMotion;
BodyMotion bodyMotion;
float flightBankTurn{};
FlightFacing flightFacing;
WingBlend wingBlend;
WingBlend fastBlend;
AirInput airInput;
JumpBoost jumpBoost;
JumpWingPreview jumpWingPreview;
bool jumpPreviewOwned{};
RE::ActiveEffect* jumpPreviewEffect{}; // identity only
std::atomic_bool groundJumpRequested{};
WingEntry wingEntry;
float blendTrace{}, blendSample{};
bool blendTarget{true}, blendMissingReported{};
RE::bhkCharacterController* flightController{}; // identity only; always reacquire before access
RE::ActiveEffect* visualEffect{}; // identity only, never dereferenced after the frame
RE::FormID flightRace{};
bool flightLordBody{};
RE::NiAVObject* visualActorRoot{}; // identity only
RE::TESObjectCELL* lastCell{};
RE::TESWorldSpace* lastWorld{};
bool lastInterior{};
RE::NiPoint3 lastPosition{};
std::string lastEvent, lastProfile;
bool supported(RE::bhkCharacterController* controller);
bool actorGraphSafe(RE::PlayerCharacter* player) {
    return player && player->Is3DLoaded() && !player->IsDead() && !player->IsInRagdollState()
        && player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) > 0;
}
bool nativeBatAnimationDriven(RE::PlayerCharacter* actor){
    bool value{};return ready&&loaded&&flight.flying()&&usesVampireLordBehavior(actor)&&actorGraphSafe(actor)
        &&actor->GetGraphVariableBool("bAnimationDriven",value)&&value;
}
bool armedFlightGate(RE::PlayerCharacter* player,bool enabled){
    if(!player||!actorGraphSafe(player)||usesVampireLordBehavior(player))return false;
    const auto controller=player->GetCharController();
    return useArmedFlightIdle(enabled,ready&&managedAirAnimation,
        player->AsActorState()->IsWeaponDrawn(),controller&&controller==flightController&&!player->IsInKillMove(),
        controller&&supported(controller));
}
bool armedIdleEligible(RE::PlayerCharacter* player){return armedFlightGate(player,config.armedFlightIdle);}
bool unarmedPoseEligible(RE::PlayerCharacter* player){
    if(!player||!actorGraphSafe(player))return false;
    const auto controller=player->GetCharController();
    return flightPoseReady()&&pose::allowed(ready&&loaded&&flight.flying(),player->AsActorState()->IsWeaponDrawn(),
        !usesVampireLordBehavior(player),controller&&controller==flightController&&!player->IsInKillMove(),
        controller&&supported(controller),config.bodyTiming.enabled);
}
bool armedSprintForbidden(RE::PlayerCharacter* player){return armedFlightGate(player,config.disableSprintWhenArmed);}

bool uiMenuOpen() {
    const auto ui = RE::UI::GetSingleton();
    return settingsMenuOpen() || !ui || ui->GameIsPaused() || ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen()
        || ui->IsModalMenuOpen() || ui->IsMenuOpen(RE::Console::MENU_NAME)
        || ui->IsMenuOpen(RE::TweenMenu::MENU_NAME);
}
bool menuOpen(){return uiMenuOpen()||flightMenuSuspended.load()||tweenHandoffPending.load();}
struct FlightMenuEvents : RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        // UI events still run while the paused player receives no updates.
        if(event&&event->menuName==RE::TweenMenu::MENU_NAME)tweenHandoffPending=true;
        return RE::BSEventNotifyControl::kContinue;
    }
} flightMenuEvents;
struct InputState {
    struct Held { bool forward{}, back{}, left{}, right{}, jump{}, down{}, sprint{}; };
    std::array<Held, 3> held{};
    float stickX{}, stickY{};
    bool flap{}, takeoff{},cycle{},land{},released{};
} input;
std::mutex inputMutex;
void clearInput() { std::lock_guard lock(inputMutex); input = {}; }
bool jumpHeld() {
    std::lock_guard lock(inputMutex);
    for(const auto& held:input.held)if(held.jump)return true;
    return false;
}

struct InputEvents : RE::BSTEventSink<RE::InputEvent*> {
    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override {
        if (!ready || !loaded || !events) { return RE::BSEventNotifyControl::kContinue; }
        if (menuOpen()) { clearInput(); return RE::BSEventNotifyControl::kContinue; }
        std::lock_guard lock(inputMutex);
        for (auto event = *events; event; event = event->next) {
            if (const auto axis = event->AsThumbstickEvent()) {
                if (axis->IsLeft()) {
                    input.stickX = axis->xValue; input.stickY = axis->yValue;
                    stick(input.stickX, input.stickY, config.deadzone);
                }
                continue;
            }
            const auto button = event->AsButtonEvent();
            if (!button) { continue; }
            const bool pad = button->device == RE::INPUT_DEVICE::kGamepad;
            const bool keyboard = button->device == RE::INPUT_DEVICE::kKeyboard;
            auto& held = input.held[pad ? 1 : keyboard ? 0 : 2];
            const auto id = pad?canonicalGamepadButton(button->GetIDCode()):button->GetIDCode();
            const std::string_view name = button->QUserEvent().c_str();
            const bool pressed = button->IsPressed();
            const bool edge = button->IsDown();
            if (name == "Forward") { held.forward = pressed; }
            if (name == "Back") { held.back = pressed; }
            if (name == "Strafe Left") { held.left = pressed; }
            if (name == "Strafe Right") { held.right = pressed; }
            if (name == "Sprint") { held.sprint = pressed; }
            if (name == "Jump") {
                bool airborneAtPress = false;
                if (edge && !held.jump) {
                    const auto player = RE::PlayerCharacter::GetSingleton();
                    const auto controller = player ? player->GetCharController() : nullptr;
                    airborneAtPress = controller && player->IsInMidair() && !supported(controller);
                }
                const auto request = classifyJumpPress(edge, held.jump, airborneAtPress);
                input.flap |= request.flap;
                input.takeoff |= request.takeoff;
                if(button->IsUp())input.released=true;
                held.jump = pressed;
            }
            auto matches = [&](unsigned key, unsigned gamepad) {
                return keyboard ? key != 0 && id == key : pad && gamepad != 0 && id == gamepad;
            };
            if (edge && matches(config.modeKey, config.padMode)) { input.cycle = true; }
            if (edge && matches(config.landKey, config.padLand)) { input.land = true; }
            if (matches(config.descendKey, config.padDescend)) { held.down = pressed; }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
} inputEvents;

Controls consumeInput() {
    std::lock_guard lock(inputMutex);
    Controls result;
    for (const auto& held : input.held) {
        result.forward += static_cast<float>(held.forward) - static_cast<float>(held.back);
        result.right += static_cast<float>(held.right) - static_cast<float>(held.left);
        result.up |= held.jump; result.down |= held.down; result.sprint |= held.sprint;
    }
    result.right += input.stickX; result.forward += input.stickY;
    result.flap = input.flap; result.takeoff = input.takeoff; result.cycle = input.cycle; result.land = input.land;
    result.jumpPressed=input.flap;result.airPress=input.takeoff;result.jumpReleased=input.released;
    input.flap = input.takeoff = input.cycle = input.land = input.released = false;
    return result;
}

bool usable(RE::PlayerCharacter* player) {
    const auto controls = RE::ControlMap::GetSingleton();
    const auto camera = RE::PlayerCamera::GetSingleton();
    return player && player->Is3DLoaded() && player->GetCharController() && !player->IsDead()
        && !player->AsActorState()->IsSwimming() && !player->IsOnMount() && !player->IsInRagdollState()
        && !player->AsActorState()->IsBleedingOut() && player->AsActorState()->GetKnockState() == RE::KNOCK_STATE_ENUM::kNormal
        && player->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal
        && controls && controls->IsMovementControlsEnabled()
        && camera && (camera->IsInFirstPerson() || camera->IsInThirdPerson());
}
void updateMenuPause(float delta) {
    if(tweenHandoffPending.exchange(false))menuFlightPause.tweenEvent();
    const auto camera=RE::PlayerCamera::GetSingleton();
    const auto controls=RE::ControlMap::GetSingleton();
    const bool transition=camera&&camera->currentState
        &&(camera->currentState->id==RE::CameraStates::kTween||camera->currentState->id==RE::CameraStates::kPCTransition);
    const bool gameplayReady=controls&&controls->IsMovementControlsEnabled()&&camera
        &&(camera->IsInFirstPerson()||camera->IsInThirdPerson());
    const bool suspended=menuFlightPause.update(delta,ready&&loaded&&flight.active(),uiMenuOpen(),transition,gameplayReady);
    const bool previous=flightMenuSuspended.exchange(suspended);
    if(previous!=suspended&&flight.active())
        SKSE::log::info("Flight menu handoff: suspended={} camera={} movementEnabled={}",suspended,
            camera&&camera->currentState?static_cast<int>(camera->currentState->id):-1,
            controls&&controls->IsMovementControlsEnabled());
}
Vec3 velocity(RE::bhkCharacterController* controller) {
    RE::hkVector4 vector;
    controller->GetLinearVelocityImpl(vector);
    return {vector.quad.m128_f32[0], vector.quad.m128_f32[1], vector.quad.m128_f32[2]};
}
bool supported(RE::bhkCharacterController* controller) {
    return controller->surfaceInfo.supportedState == RE::hkpSurfaceInfo::SupportedState::kSupported
        && controller->surfaceInfo.surfaceNormal.quad.m128_f32[2] > 0.45f;
}
bool muteAirFootEvent(const RE::BSAnimationGraphEvent* event){
    if(!ready||!loaded||!config.muteFlightFootsteps||!event||!event->tag.c_str())return false;
    const auto player=RE::PlayerCharacter::GetSingleton();
    if(!player||event->holder!=player||!actorGraphSafe(player))return false;
    const auto controller=player->GetCharController();
    if(!controller||supported(controller)||(!managedAirAnimation&&!player->IsInMidair()))return false;
    const std::string_view tag=event->tag.c_str();
    const std::string_view payload=event->payload.c_str()?event->payload.c_str():"";
    const auto sound=directSoundName(tag,payload);
    if(!tag.starts_with("Foot")&&sound.empty())return false;
    return airborneFootAudio(true,true,true,tag,payload,registeredFootContact(tag),
        !sound.empty()&&!sound.starts_with("FST")&&registeredFootSound(sound));
}
bool muteNativeWingEvent(const RE::BSAnimationGraphEvent* event){
    if(!ready||!loaded||!flight.active()||!event||!event->tag.c_str())return false;
    const auto p=RE::PlayerCharacter::GetSingleton();if(event->holder!=p||!usesVampireLordBehavior(p))return false;
    const auto payload=event->payload.c_str()?event->payload.c_str():"";
    return directSoundName(event->tag.c_str(),payload)=="NPCVampireLordWing";
}
struct AirborneExitEvents : RE::BSTEventSink<RE::BSAnimationGraphEvent> {
    AirborneExit window;
    std::mutex mutex;
    std::atomic_uint samples{};
    float registrationWait{};
    void reset(){std::lock_guard lock(mutex);window.reset();registrationWait=0;}
    void registerGraphs(RE::PlayerCharacter* player){
        RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
        if(!player||!player->GetAnimationGraphManager(manager)||!manager)return;
        RE::BSSpinLockGuard lock(manager->GetRuntimeData().updateLock);
        // Source filtering also prevents footstep particles and mod listeners.
        for(const auto& graph:manager->graphs)if(graph&&graph->holder==player)
            graph->GetEventSource<RE::BSAnimationGraphEvent>()->PrependEventSink(this);
    }
    void begin(RE::PlayerCharacter* player,bool fullFlight){
        const auto controller=actorGraphSafe(player)?player->GetCharController():nullptr;
        {
            std::lock_guard lock(mutex);
            window.begin(fullFlight,controller&&controller==flightController,
                !controller||supported(controller),reinterpret_cast<std::uintptr_t>(controller));
        }
        if(!fullFlight||!controller||supported(controller))return;
        // Filter at each player's graph source, before native and mod subscribers.
        // A player-only consumer filter cannot prevent footstep particle listeners.
        registerGraphs(player);
    }
    void update(RE::PlayerCharacter* player,float dt){
        const auto controller=actorGraphSafe(player)?player->GetCharController():nullptr;
        {
            std::lock_guard lock(mutex);
            window.update(dt,controller!=nullptr,!controller||supported(controller),reinterpret_cast<std::uintptr_t>(controller));
        }
        // Refresh after graph replacement or an external mod's new subscriber.
        // Grounded players need no registration work and always pass the filter.
        registrationWait=std::max(0.0f,registrationWait-(std::isfinite(dt)?std::max(0.0f,dt):0.0f));
        if(ready&&loaded&&(config.muteFlightFootsteps||(flight.active()&&usesVampireLordBehavior(player)))&&controller&&!supported(controller)
            &&(managedAirAnimation||player->IsInMidair())&&registrationWait<=0){
            registerGraphs(player);registrationWait=0.25f;
        }
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>*)override{
        if(!ready||!loaded||!event||!event->tag.c_str())return RE::BSEventNotifyControl::kContinue;
        const auto player=RE::PlayerCharacter::GetSingleton();
        const bool ours=player&&event->holder==player;
        const auto controller=ours&&actorGraphSafe(player)?player->GetCharController():nullptr;
        bool block=muteAirFootEvent(event)||muteNativeWingEvent(event),observe=false;
        {
            std::lock_guard lock(mutex);
            block=block||window.suppress(ours,controller!=nullptr,!controller||supported(controller),
                reinterpret_cast<std::uintptr_t>(controller),event->tag.c_str());
            const std::string_view tag=event->tag.c_str();
            observe=ours&&window.owns(controller!=nullptr,!controller||supported(controller),
                reinterpret_cast<std::uintptr_t>(controller))&&(tag.starts_with("Foot")||tag.starts_with("JumpLand"));
        }
        if(observe&&samples.fetch_add(1)<24)SKSE::log::info("Airborne exit graph event: {} suppressed={} supported=false",event->tag.c_str(),block);
        else if(block&&samples.fetch_add(1)<48)SKSE::log::info("Suppressed airborne foot audio: tag={} payload={}",event->tag.c_str(),event->payload.c_str()?event->payload.c_str():"");
        if(!block)return RE::BSEventNotifyControl::kContinue;
        return RE::BSEventNotifyControl::kStop;
    }
} airborneExitEvents;
void clearFall(RE::bhkCharacterController* controller) {
    RE::hkVector4 position;
    controller->GetPositionImpl(position, false);
    controller->fallTime = 0;
    // Controller position is in Havok units; fallStartHeight is in Skyrim units.
    controller->fallStartHeight = fallHeightInGameUnits(position.quad.m128_f32[2], RE::bhkWorld::GetWorldScaleInverse());
}
bool publish(RE::PlayerCharacter* player, bool flying) {
    updateFlightBreath(player,flying&&!usesVampireLordBehavior(player)&&player->GetCharController()&&!supported(player->GetCharController()));
    const bool written = player->SetGraphVariableBool("Wingflight_IsFlying", flying);
    const bool poses = flying && !usesVampireLordBehavior(player) && (config.poseWhenArmed || !player->AsActorState()->IsWeaponDrawn());
    player->SetGraphVariableBool("Wingflight_UsePose", poses&&player->AsActorState()->IsWeaponDrawn());
    player->SetGraphVariableBool("Wingflight_ForwardPose", poses && bodyMotion.forward());
    player->SetGraphVariableInt("Wingflight_BodyPose",bodyMotion.pose());
    player->SetGraphVariableBool("Wingflight_BodyPoses", poses && config.bodyTiming.enabled);
    return written;
}
void pay(RE::Actor* player, float cost) {
    if (cost > 0) { player->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -cost); }
}
void finish(RE::PlayerCharacter* player, WingMatch match, const char* reason, bool freeFall=false) {
    jumpWingPreview.reset();jumpPreviewOwned=false;jumpPreviewEffect=nullptr;
    flightCollision.reset();
    if (flight.active() || !lastProfile.empty()) {
        SKSE::log::info("Flight ended: {}", reason);
    }
    // Register the cosmetic guard before publishing the pose change or JumpFall.
    // Keep native gravity, falling state messages and damage completely native.
    airborneExitEvents.begin(player,freeFall&&managedAirAnimation);
    // Release ownership before invoking any graph callbacks. Never notify a dying
    // model or write into a ragdoll controller as part of flight cleanup.
    nativeBatTravel.reset();nativeTravelWasPaused=false;pauseVampireWingOverlay(false);
    nativeBatVelocitySamples=0;
    flightFacing.release();bodyMotion.reset();flightBankTurn=0;
    ownedGravity.release();
    flight.reset(); lastEvent.clear(); lastProfile.clear(); flightRace = 0; flightLordBody=false;visualActorRoot=nullptr;
    setGlideClipGuard(nullptr,false);
    managedAirAnimation = false; wings.clearVisual(actorGraphSafe(player)); bodyRetry = 0;
    if(actorGraphSafe(player))updateFlightBreath(player,false);
    wingMotion.reset(); wingRetry = ceilingTimer = 0; flightController = nullptr; visualEffect = nullptr;
    if (actorGraphSafe(player)) {
        if (!match.profile) { match = wings.find(player); }
        updateHeldWings(match,0,jumpHeld(),freeFall,false,false,false,config.glideHoldSeconds);
        if (match.profile) {
            wings.setBlend(match,0,0);
            wings.send(match, freeFall?match.profile->fall:match.profile->land);
        }
        publish(player, false);
        if (freeFall) { player->NotifyAnimationGraph(RE::BSFixedString("JumpFall")); }
    } else { resetGraphs = true;resetHeldWings(); }
}
float cameraYaw(RE::PlayerCharacter* player) {
    const auto camera = RE::PlayerCamera::GetSingleton();
    if (!camera || !camera->currentState) { return player->GetAngleZ(); }
    if (camera->IsInThirdPerson()) {
        const auto state=static_cast<RE::ThirdPersonState*>(camera->currentState.get());
        const float heading=player->GetAngleZ()+state->freeRotation.x;
        if(std::isfinite(heading))return heading;
    }
    RE::NiQuaternion q;
    camera->currentState->GetRotation(q);
    const float yaw = -std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
    return std::isfinite(yaw) ? yaw : player->GetAngleZ();
}
const char* modeName(Mode mode) {
    switch (mode) {
    case Mode::flap: return "Wingflight: flap / glide";
    case Mode::free: return "Wingflight: free flight";
    default: return "Wingflight: flap / glide";
    }
}
void start(RE::PlayerCharacter* player, WingMatch match, bool restoring,bool glideOnly=false) {
    flightCollision.reset();
    if(!wings.prepareVisual(match).found){
        SKSE::log::error("Flight blocked: original wing resources could not be bound by the runtime driver");
        RE::SendHUDMessage::ShowHUDMessage("Wingflight: original wing animations unavailable; disable older Wingflight resource patches");return;
    }
    bool graphValue{};
    if (!player->GetGraphVariableBool("Wingflight_IsFlying", graphValue)) {
        SKSE::log::error("Flight blocked: BDI graph variable missing");
        RE::SendHUDMessage::ShowHUDMessage("Wingflight: Behavior Data Injector variable missing"); return;
    }
    const auto controller = player->GetCharController();
    if (!restoring && !glideOnly && !match.profile->continuousFlight && player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) < config.tuning.flapCost) { return; }
    const auto initialVelocity = velocity(controller);
    if (!std::isfinite(initialVelocity.x) || !std::isfinite(initialVelocity.y) || !std::isfinite(initialVelocity.z)) {
        SKSE::log::error("Flight blocked: invalid controller velocity x={} y={} z={} controller={:X}",
            initialVelocity.x,initialVelocity.y,initialVelocity.z,reinterpret_cast<std::uintptr_t>(controller)); return;
    }
    airborneExitEvents.reset();
    bodyMotion.reset();jumpBoost.reset();flightBankTurn=0;
    ownedGravity.release();
    if(glideOnly)flight.beginGlide(initialVelocity,config.tuning);else flight.begin(initialVelocity, !restoring, config.tuning);
    // Keep the preview's displayed wing weights in the runtime driver. Its
    // entry blend will continue from them, rather than going back to folded.
    jumpWingPreview.reset();jumpPreviewOwned=false;jumpPreviewEffect=nullptr;
    updateAWUCompatibility(player,true);
    if(!glideOnly)flight.restoreMode(restoring?savedMode(resumeMode):preferredFlightMode);
    flight.setContinuousFlight(!glideOnly&&match.profile->continuousFlight);
    wings.prepareContinuousAnimation(match,true,false,config.bodyTiming.forwardGlideSpeed);
    if (const auto race = player->GetRace()) { flightRace = race->GetFormID(); }
    flightLordBody=usesVampireLordBehavior(player);visualActorRoot=player->Get3D(false);
    lastCell = player->GetParentCell(); lastPosition = player->GetPosition();
    lastWorld = player->GetWorldspace(); lastInterior = lastCell && lastCell->IsInteriorCell();
    wingMotion.reset(); if(!glideOnly&&!match.profile->continuousFlight)wingMotion.pulse(!restoring,false,config.tuning.flapCost); wingBlend.reset(glideOnly?0.0f:1.0f);fastBlend.reset(0); wingEntry.reset();
    blendTarget=true; blendTrace=0.8f; blendSample=0; blendMissingReported=false;
    wingRetry = ceilingTimer = 0; flightController = controller; visualEffect = match.effect;
    lastEvent.clear(); lastProfile = match.profile->id;
    managedAirAnimation = !glideOnly;
    wings.prepareVisual(match);
    wings.setFlapRate(0.01f);
    wings.setBlend(match,1.0f,0.0f);
    // Set OAR conditions before entering its idle/locomotion slots. Ending the
    // jump state skips the landing clip; it does not claim physical ground.
    // Outside-flight hold-glide controls only the wings and descent speed.
    // Full flight, including its own glide, retains the managed body poses.
    publish(player, flight.flying());
    // JumpLandEnd can activate locomotion immediately during takeoff. Register
    // the owned character before that activation, including a sprint from ground.
    prepareArmedSprintIdle(player,!glideOnly&&config.armedFlightIdle&&!usesVampireLordBehavior(player));
    prepareFlightPose(player,!glideOnly&&config.bodyTiming.enabled&&!usesVampireLordBehavior(player));
    // Enter the isolated wing state before the body exit can relay ground events.
    if (wings.send(match, match.profile->takeoff)) { lastEvent = match.profile->takeoff; }
    const char* bodyEvent=usesVampireLordBehavior(player)?"JumpStandingEnd":"JumpLandEnd";
    const bool bodyAccepted = !glideOnly && player->NotifyAnimationGraph(RE::BSFixedString(bodyEvent));
    bodyRetry = 0.25f;
    SKSE::log::info("Flight body handoff {} accepted={} armedPoses={}",bodyEvent, bodyAccepted, config.poseWhenArmed&&!usesVampireLordBehavior(player));
    wingRetry = 0.20f; // Private graph blends into the raised-wing pose first.
    if(!glideOnly)RE::SendHUDMessage::ShowHUDMessage(flight.continuousFlight()?"Wingflight: continuous flight":modeName(flight.mode()));
    SKSE::log::info("Flight started adapter={} restored={} glideOnly={} runtime={} body={} wingBackend={} continuousFlight={}", lastProfile, restoring,glideOnly,runtimeName(),usesVampireLordBehavior(player)?"VampireLord":"Character",match.profile->animationless?"logical invisible":match.profile->continuousFlight?"provider loop":match.profile->nativeVampire?"skeletal":"effect ART",flight.continuousFlight());
}
void update(RE::PlayerCharacter* player, float delta) {
    if (!ready || !loaded || !player) { return; }
    if (!actorGraphSafe(player)) {
        jumpWingPreview.reset();jumpPreviewOwned=false;jumpPreviewEffect=nullptr;
        if (flight.active()) { finish(player, {}, "dead, ragdoll or unloaded actor; no controller/graph writes"); }
        else wings.clearVisual();
        clearInput(); return;
    }
    refreshVampireContext(player);
    updateAWUCompatibility(player,config.glideEnabled || flight.active());
    if(player->AsActorState()->IsWeaponDrawn() && !player->AsActorState()->IsSwimming())
        awuAnimationEvent(player,RE::BSFixedString("TailCombatIdle"));
    if (resetGraphs && player->Is3DLoaded()) {
        bool value{};
        const bool hasFlight = player->GetGraphVariableBool("Wingflight_IsFlying", value);
        const bool hasPose = player->GetGraphVariableBool("Wingflight_UsePose", value);
        SKSE::log::info("Initial BDI graph check: IsFlying={} UsePose={}", hasFlight, hasPose);
        // Release a private art state left in a loaded save or an unsafe exit.
        // Fresh ownership is checked; no pointer from the old session is used.
        const auto resetMatch=wings.find(player);
        if (resetMatch.profile) { wings.setBlend(resetMatch,0,0); wings.send(resetMatch, resetMatch.profile->land); }
        publish(player, false); resetGraphs = false;
    }
    const bool wasActive = flight.active();
    auto match = wings.find(player,wasActive);
    // Inventory/camera-tween states may temporarily disable movement controls.
    // Suspend before applying the gameplay-control gate, preserving an airborne save.
    if (menuOpen()) {
        flightCollision.reset();
        clearInput();
        if (wasActive && player->GetCharController() && match.profile && !player->IsDead()
            && !player->AsActorState()->IsSwimming() && !player->IsOnMount()
            && RE::UI::GetSingleton() && !RE::UI::GetSingleton()->GameIsPaused() && !nativeBatTravel.yielding()) {
            clearFall(player->GetCharController());
            player->GetCharController()->SetLinearVelocityImpl(RE::hkVector4(0, 0, 0, 0));
        }
        return;
    }
    const bool nativeTravel=wasActive&&usesVampireLordBehavior(player)&&nativeBatTravel.yielding();
    const bool safeTravelActor=nativeTravel&&!player->AsActorState()->IsSwimming()&&!player->IsOnMount()
        &&!player->AsActorState()->IsBleedingOut()&&player->AsActorState()->GetKnockState()==RE::KNOCK_STATE_ENUM::kNormal
        &&player->AsActorState()->GetSitSleepState()==RE::SIT_SLEEP_STATE::kNormal&&!player->IsInKillMove();
    if ((!usable(player)&&!safeTravelActor) || (wasActive && player->GetRace() && player->GetRace()->GetFormID() != flightRace)) {
        if(wasActive){
            const auto controls=RE::ControlMap::GetSingleton();const auto camera=RE::PlayerCamera::GetSingleton();
            diagnostic::report("flight.actor-eligibility-lost","finish flight",player,
                fmt::format("3D={} dead={} swim={} mount={} ragdoll={} bleedOut={} knock={} sitSleep={} movement={} camera={} race={:08X} expectedRace={:08X}",
                    player->Is3DLoaded(),player->IsDead(),player->AsActorState()->IsSwimming(),player->IsOnMount(),player->IsInRagdollState(),
                    player->AsActorState()->IsBleedingOut(),static_cast<unsigned>(player->AsActorState()->GetKnockState()),
                    static_cast<unsigned>(player->AsActorState()->GetSitSleepState()),controls&&controls->IsMovementControlsEnabled(),
                    camera&&camera->currentState?static_cast<int>(camera->currentState->id):-1,
                    player->GetRace()?player->GetRace()->GetFormID():0,flightRace));
        }
        if (wasActive) { finish(player, match, "actor unavailable, transformed, swimming, mounted or controls disabled"); }
        else if(jumpPreviewOwned){wings.clearVisual(true);jumpWingPreview.reset();jumpPreviewOwned=false;jumpPreviewEffect=nullptr;}
        clearInput(); return;
    }
    const auto controller = player->GetCharController();
    if (wasActive && controller != flightController) {
        diagnostic::report("flight.controller-replaced","finish flight without adopting replacement",player,
            fmt::format("previous={:X} current={:X}",reinterpret_cast<std::uintptr_t>(flightController),reinterpret_cast<std::uintptr_t>(controller)));
        finish(player, {}, "controller replaced"); clearInput(); return;
    }
    const auto cell = player->GetParentCell();
    const bool interiorTransition = lastCell != cell && (lastInterior || (cell && cell->IsInteriorCell()));
    if (wasActive && (interiorTransition || lastWorld != player->GetWorldspace()
        || player->GetPosition().GetDistance(lastPosition) > 4096.0f)) {
        finish(player, match, "cell transition or teleport"); clearInput(); return;
    }
    if (wasActive && !match.profile) { finish(player, {}, "wings removed or selected provider unavailable"); clearInput(); return; }
    if(wasActive&&flightLordBody!=usesVampireLordBehavior(player)){finish(player,{},"body graph changed during transformation");clearInput();return;}
    if (!std::isfinite(delta) || delta <= 0) {
        if(!std::isfinite(delta)||delta<0)diagnostic::report("player.delta-invalid","skip Wingflight update",player,{},delta);
        return; }
    const float dt = std::min(delta, 0.1f);
    if(nativeTravel){
        flightCollision.reset();
        if(!nativeTravelWasPaused)SKSE::log::info("Native Vampire Lord bat travel: yielding velocity, controller state, facing and wing pose to the original skill");
        nativeTravelWasPaused=true;flightFacing.release();flightBankTurn=0;clearInput();
        lastCell=player->GetParentCell();lastPosition=player->GetPosition();lastWorld=player->GetWorldspace();
        lastInterior=lastCell&&lastCell->IsInteriorCell();
        return; // No simulation, lift/cost, wing clock, velocity or InAir writes.
    }
    if(nativeTravelWasPaused){
        flightCollision.reset();
        nativeTravelWasPaused=false;
        const auto height=player->GetPosition().z/RE::bhkWorld::GetWorldScaleInverse();
        if(!flight.resumeAfterNativeTravel(height)){finish(player,match,"invalid bat-travel destination");return;}
        wingMotion.reset();wingEntry.reset();ceilingTimer=0;bodyRetry=0.5f;
        SKSE::log::info("Native Vampire Lord bat travel: resumed existing mode={} at actual destination Z={:.2f}",static_cast<int>(flight.mode()),player->GetPosition().z);
    }
    if(wasActive&&(lastProfile!=match.profile->id||visualEffect!=match.effect||visualActorRoot!=player->Get3D(false))){
        SKSE::log::info("Flight wing provider rebind: previous={} next={} body={} backend={}; saved ordinary mode preference retained",lastProfile,match.profile->id,usesVampireLordBehavior(player)?"VampireLord":"Character",match.profile->animationless?"logical invisible":match.profile->continuousFlight?"provider loop":match.profile->nativeVampire?"skeletal":"effect ART");
        wings.clearVisual();lastEvent.clear();wingMotion.reset();if(!flight.glideOnly()&&!match.profile->continuousFlight)wingMotion.pulse(false);
        wingBlend.reset();fastBlend.reset(0);wingEntry.reset();blendTarget=true;blendTrace=0.8f;blendSample=0;blendMissingReported=false;
        visualEffect=match.effect;lastProfile=match.profile->id;visualActorRoot=player->Get3D(false);wingRetry=0;
    }
    wings.advanceVisual(dt);
    if(!heldWingGateInstalled()||!heldWingsEligible(match.profile))wings.updateGroundRecovery(match,dt,supported(controller),flight.active());
    if (resumeMode >= 0) {
        resumeWait += dt;
        if (match.profile) {
            if (!supported(controller) && (player->IsInMidair()
                || controller->context.currentState == RE::hkpCharacterStateTypes::kInAir)) {
                start(player, match, true); resumeMode = -1;
            } else if (supported(controller)) { resumeMode = -1; }
        }
        if (resumeWait > 5.0f && resumeMode >= 0) { resumeMode = -1; SKSE::log::warn("Saved flight not resumed: wing/controller state unavailable"); }
    }
    auto controls = consumeInput();
    const bool providerLoop=match.profile&&match.profile->continuousFlight;
    if(flight.flying()&&flight.continuousFlight()!=providerLoop) {
        flight.setContinuousFlight(providerLoop);
        if(!providerLoop)flight.restoreMode(preferredFlightMode);
    }
    if(providerLoop&&flight.flying())controls.cycle=controls.flap=false;
    controls.yaw = cameraYaw(player);
    const bool sprintHeld=controls.sprint;
    controls.sprint=flightSprintAllowed(player->AsActorState()->IsSprinting(),armedSprintForbidden(player));
    controls.pitch=player->GetAngleX();
    if(const auto cam=RE::PlayerCamera::GetSingleton();cam&&cam->IsInThirdPerson())
        controls.pitch+=static_cast<RE::ThirdPersonState*>(cam->currentState.get())->freeRotation.y;
    controls.pitch=std::clamp(controls.pitch,-1.50f,1.50f);
    // While holding a glide, physical support is authoritative. Body graph
    // transitions must not turn a held key into repeated exit/re-entry cycles.
    const bool airborne=(flight.glideOnly() || player->IsInMidair())&&!supported(controller);
    if(controls.land){airInput.cancel(controls.up);}
    const auto action=airInput.update(dt,controls.up,controls.jumpPressed,controls.jumpReleased,controls.airPress,
        airborne,flight.flying(),config.glideHoldSeconds);
    controls.glideHeld=action.glide && config.glideEnabled;
    controls.takeoff=action.takeoff;
    if(groundJumpRequested.exchange(false)){jumpBoost.arm();jumpWingPreview.arm();}
    if(!flight.active()){
        auto native=velocity(controller);
        const float boosted=jumpBoost.sample(dt,match.profile!=nullptr,airborne,native.z,config.jumpHeightMultiplier);
        if(boosted!=native.z)controller->SetLinearVelocityImpl(RE::hkVector4(native.x,native.y,boosted,0));
    }
    if (!flight.active() && match.profile && controls.takeoff && airborne) {
        start(player, match, false);
        controls.flap = false; // Initial animation beat is already queued; no immediate lift.
    }
    if(config.glideEnabled&&!flight.active()&&match.profile&&action.glide&&!controls.land)start(player,match,false,true);
    if (!flight.active()) {
        const bool previewEligible=heldWingGateInstalled()&&heldWingsEligible(match.profile);
        if(jumpPreviewOwned&&(!previewEligible||jumpPreviewEffect!=match.effect)){
            wings.clearVisual(true);jumpPreviewOwned=false;jumpPreviewEffect=nullptr;jumpWingPreview.reset();
        }
        const float preview=jumpWingPreview.update(dt,airborne,previewEligible,config.jumpWingOpenPercent,
            config.jumpWingOverOpenPercent,config.jumpWingOpenSeconds,config.jumpWingSettleSeconds);
        if(preview>0&&previewEligible){
            if(wings.setJumpPreview(match,preview,config.jumpWingFlutterPeriod,config.jumpWingFlutterAmplitude)){jumpPreviewOwned=true;jumpPreviewEffect=match.effect;}
        }else if(jumpPreviewOwned){wings.clearVisual(false);jumpPreviewOwned=false;jumpPreviewEffect=nullptr;}
        flightFacing.release();return;
    }
    const auto previousMode = flight.mode();
    const auto observedVelocity = velocity(controller);
    if(!std::isfinite(observedVelocity.x)||!std::isfinite(observedVelocity.y)||!std::isfinite(observedVelocity.z))
        diagnostic::report("flight.measured-velocity-nonfinite","retain existing simulation policy",controller,
            fmt::format("x={} y={} z={}",observedVelocity.x,observedVelocity.y,observedVelocity.z));
    const auto position = player->GetPosition();
    // Inspect last physics step before landing can release flight ownership.
    bool collided=false;
    if(flight.flying()&&!controls.land)collided=flightCollision.update(player,controller,delta,config.collision);
    else flightCollision.reset();
    if(collided){
        const bool collisionAirborne=actorGraphSafe(player)&&player->GetCharController()==controller&&!supported(controller);
        airInput.cancel(controls.up);
        finish(player,match,"damaging collision: release to native falling",collisionAirborne);
        clearInput();return; // Never reapply this frame's commanded flight velocity.
    }
    if(!actorGraphSafe(player)||player->GetCharController()!=controller){
        finish(player,match,"collision damage changed actor/controller state");clearInput();return;
    }
    // A stopped upward controller beneath a ceiling should not store thrust indefinitely.
    if (flight.velocity().z > 0.5f && observedVelocity.z <= 0.05f && std::abs(position.z - lastPosition.z) < 0.01f) {
        ceilingTimer += dt;
        if (ceilingTimer > 0.2f) { flight.ceilingContact(); }
    } else { ceilingTimer = 0; }
    // Predict raising lead from the visible clip. Our shared simulated gravity
    // controls momentum; lift still requires real downstroke progress.
    const auto hoverPlayback=(flight.mode()==Mode::free || controls.cycle)?wings.prepareVisual(match):WingPlayback{};
    const auto hoverTiming=config.hoverTiming.apply(match.profile->timing);
    const bool nextFree=controls.cycle?flight.mode()!=Mode::free:flight.mode()==Mode::free;
    const bool nextFast=controls.sprint&&std::hypot(controls.right,controls.forward)>0.05f;
    const bool cancelAutomatic=!nextFree||nextFast||controls.down;
    auto flightTuning=config.tuning;
    if(providerLoop)flightTuning.continuousRiseSpeed=match.profile->continuousRiseSpeed;
    const auto result = flight.tick(dt, controls,
        {match.profile != nullptr, true, supported(controller), player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina),
            position.z/RE::bhkWorld::GetWorldScaleInverse(),hoverPlayback.active?hoverPlayback.phase:-1.0f,
            hoverPlayback.flapWeight,match.profile->timing.downstrokeEnd,
            wingMotion.automaticLead(hoverTiming,hoverPlayback.phase),hoverTiming.downstrokeSeconds,wingMotion.pending()>0,
            wingMotion.reservedCost(true,cancelAutomatic),wingMotion.reservedCost(false,cancelAutomatic)}, flightTuning);
    pay(player, result.cost);
    if (result.released) {
        airInput.cancel(controls.up);
        finish(player,match,"manual exit: native free fall and AWU falling wings",!supported(controller));
        return; // No velocity, fall-time or fall-height writes after releasing.
    }
    if (result.landed || result.aborted) { airInput.cancel(controls.up);finish(player, match, result.landed ? "ground contact" : "eligibility lost"); return; }
    if (previousMode != flight.mode()) {
        if(controls.cycle && flight.flying()&&!flight.continuousFlight())preferredFlightMode=flight.mode();
        RE::SendHUDMessage::ShowHUDMessage(modeName(flight.mode()));
    }
    const float heading=player->GetAngleZ();
    const float observedForward=observedVelocity.x*std::sin(heading)+observedVelocity.y*std::cos(heading);
    bool continuous = flight.automaticHover();
    const bool nativeSprintLoop=vampire::largeSprintLoop(match.profile->nativeVampire,flight.flying(),flight.highSpeed());
    const auto timing=((flight.mode()==Mode::free&&!flight.highSpeed()&&!flight.glideOnly())||nativeSprintLoop)
        ?config.hoverTiming.apply(match.profile->timing):match.profile->timing;
    const float bodyTurn=std::remainder(controls.yaw-heading,6.2831853f)+controls.right*0.5f;
    auto playback=wings.prepareVisual(match);
    if(!playback.found){finish(player,match,"selected wing animation resources unavailable");return;}
    wings.prepareContinuousAnimation(match,continuousWingVisualActive(flight.flying(),flight.glideOnly(),supported(controller)),
        flight.flying()&&flight.highSpeed(),config.bodyTiming.forwardGlideSpeed,dt);
    bodyMotion.update(dt,!player->AsActorState()->IsWeaponDrawn(),controls.forward,
        observedForward,config.tuning.speed,config.bodyTiming,flight.highSpeed());
    if(!providerLoop) {
    wingMotion.setDescentGlide(flight.descentGlide());
    wingMotion.setHover(continuous);
    if(nativeSprintLoop)wingMotion.setHover(true);
    if (canParkFlap(playback.graphWeight,playback.flapWeight,playback.active,playback.gliding)
        && (result.flapped || playback.phase>0.001f) && wings.parkInvisibleFlap()) {
        // Re-sample instead of assuming an engine method took effect.
        playback=wings.prepareVisual(match);
    }
    if (canParkFlap(playback.graphGlideWeight,playback.glideWeight,playback.gliding,playback.active)
        && wings.parkInvisibleGlide()) {
        playback=wings.prepareVisual(match);
    }
    bool reusedEntryDownstroke=false;
    if (result.flapped) {
        if(result.highSpeedEntryFlap)reusedEntryDownstroke=wingMotion.highSpeedEntryPulse(playback.phase,playback.flapWeight,timing,config.tuning.flapCost);
        else wingMotion.pulse(true,flight.mode()==Mode::free&&!flight.highSpeed(),config.tuning.flapCost);
    }
    if(result.automaticFlapped){
        wingMotion.automaticPulse(config.tuning.autoFlapCost);
        if(config.logSamples)SKSE::log::info("Automatic hover beat: baseZ={:.2f} heightZ={:.2f} nativeVz={:.3f} gravity={:.3f} impulse={:.3f} cost={:.2f}",
            flight.hoverTarget()*RE::bhkWorld::GetWorldScaleInverse(),position.z,observedVelocity.z,config.tuning.gravity,config.tuning.autoFlapImpulse,config.tuning.autoFlapCost);
    }
    if (result.highSpeedEntryFlap) {
        SKSE::log::info("High-speed entry Space-equivalent beat accepted: multiplier={:.3f} pendingStrokes={} reuseDownstroke={} phase={:.4f} raisedThreshold=0.70",flight.speedMultiplier(),wingMotion.pending(),reusedEntryDownstroke,playback.phase);
    }
    // A visual-only credit repeats the large stroke in native high-speed flight.
    // It supplies neither impulse nor stamina payment/extra speed. Space still
    // queues its normal physical/manual credit and sprint acceleration.
    if(nativeSprintLoop&&wingMotion.pending()==0)wingMotion.pulse(false);
    // Resetting a hidden sample clock must never block input or blend progress.
    wingMotion.update(dt, {playback.active, playback.gliding, playback.phase, playback.duration, flight.velocity().z,playback.flapWeight,playback.frequency,
            flight.automaticRecoverySeconds(timing.upstrokeSeconds,timing.downstrokeSeconds,config.tuning),flight.highSpeed(),config.highSpeedFlapHoldSeconds},
        continuous||nativeSprintLoop, timing);
    wings.updatePowerSound(match,playback);
    const auto charge=wingMotion.charge();
    if(charge.due){
        const float stamina=player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
        if(!std::isfinite(stamina))diagnostic::report("stamina.value-nonfinite","retain current downstroke payment rule",player,{},stamina,charge.cost);
        const bool paid=charge.cost<=0||(std::isfinite(stamina)&&stamina>=charge.cost);
        if(paid)pay(player,charge.cost);
        wingMotion.settleCharge(paid);
        if(!paid&&charge.kind==WingMotion::Credit::automatic){
            flight.rejectAutomaticStroke(config.tuning);continuous=flight.automaticHover();wingMotion.setHover(continuous);
            RE::SendHUDMessage::ShowHUDMessage(modeName(flight.mode()));
        }
        if(config.logSamples&&(charge.cost>0||!paid))SKSE::log::info("Downstroke stamina: type={} phase={:.4f} cost={:.2f} paid={} staminaBefore={:.2f}",
            charge.kind==WingMotion::Credit::automatic?"automatic":"manual",playback.phase,charge.cost,paid,stamina);
    }
    flight.applyWingLift(wingMotion.liftFraction(),wingMotion.liftStartsStroke(),config.tuning);
    flight.applyWingLift(wingMotion.automaticLiftFraction(),wingMotion.automaticLiftStartsStroke(),config.tuning,true);
    if (!continuous && wingMotion.liftStartsStroke() && config.logSamples) {
        SKSE::log::info("Wing lift begins: phase={:.4f} visible={:.4f} fraction={:.4f} velocityZ={:.3f}",
            playback.phase,playback.flapWeight,wingMotion.liftFraction(),flight.velocity().z);
    }
    wingEntry.update(dt,playback.active && playback.gliding && playback.spreadActive,std::min(match.profile->timing.expandSeconds,0.40f));
    const bool parkedGlide=!wingMotion.wantsFlap() && wingBlend.value()==0;
    wings.setFlapRate((parkedGlide || !wingEntry.complete())?0.01f:wingMotion.rate());
    const bool wantsFlap=!flight.glideOnly()&&(continuous || nativeSprintLoop || wingMotion.wantsFlap());

    if (wantsFlap!=blendTarget) {
        blendTarget=wantsFlap; blendTrace=0.8f; blendSample=0;
        SKSE::log::info("Wing blend target={} startWeight={:.4f}", wantsFlap?"flap":"glide",wingBlend.value());
    }
    // The two private Gamebryo generators remain active. Only their shared
    // percentage changes, including when Space reverses an unfinished blend.
    if (playback.active || playback.gliding) {
        wingBlend.update(dt,wantsFlap,wantsFlap?match.profile->timing.toFlapSeconds*wingMotion.raiseRatio(match.profile->timing):match.profile->timing.toGlideSeconds);
    }
    // Incoming graph percentages can lead the displayed Ni contribution.
    // Use real visibility to avoid consuming the open pose while obscured.
    wings.updateGlideRate(bodyMotion.glideSpeed(),config.bodyTiming.glideSpeed);
    fastBlend.update(dt,flight.highSpeed(),0.50f);
    wings.updatePowerPitch(config.powerStrokeWingPitch*fastBlend.value());
    const bool blendWritten=wings.setBlend(match,wingBlend.value(),wingEntry.value(),fastBlend.value());
    if (!blendWritten && !blendMissingReported) {
        SKSE::log::warn("Runtime wing command unavailable; inspect original provider/model binding");
        blendMissingReported=true;
    }
    if (config.logSamples && blendTrace>0) {
        blendTrace-=dt; blendSample-=dt;
        if (blendSample<=0) {
            blendSample=0.05f;
            SKSE::log::info("Wing blend trace: modeFlap={:.4f} entry={:.4f} observedGraph={:.4f} NiFlap={:.4f} NiFast={:.4f} NiSpread={:.4f} otherWeight={:.4f} active={}/{} phase={:.3f} rate={:.3f} written={} sequences={}",
                wingBlend.value(),wingEntry.value(),playback.graphWeight,playback.flapWeight,playback.glideWeight,playback.spreadWeight,playback.otherWeight,
                playback.active,playback.gliding,playback.phase,playback.frequency,blendWritten,playback.activeSequence);
            SKSE::log::info("Wing fast-flap phase={:.4f} configuredEntry={:.4f} rate={:.4f} hover={}",playback.glidePhase,match.profile->timing.glideEntryPhase,playback.glideFrequency,continuous);
            if(match.profile->nativeVampire)SKSE::log::info("Native Vampire Lord animation: largeSprintLoop={} flutterPhase={:.3f} flutterRate={:.3f} flutterWeight={:.3f}",nativeSprintLoop,playback.spreadPhase,playback.spreadFrequency,playback.spreadWeight);
        }
    }
    const auto& event = continuous ? match.profile->hover : wingMotion.wantsFlap() ? match.profile->flap : match.profile->glide;
    const bool actuallyPlaying=wantsFlap?playback.active:playback.gliding;
    wingRetry = std::max(0.0f, wingRetry - dt);
    // Acceptance is not playback. Reassert a request if its real sequence never
    // activates or is displaced. Private wildcards cannot restart their own state.
    if ((event != lastEvent || !actuallyPlaying) && wingRetry <= 0) {
        if (wings.send(match, event)) { lastEvent = event; lastProfile = match.profile->id; }
        wingRetry = 0.25f;
    }
    } // Provider-owned loops retain native playback, sound and wing-root transforms.
    if (!publish(player, flight.flying())) {
        diagnostic::report("flight.graph-write-failed","finish flight",player,"Wingflight_IsFlying");
        finish(player, match, "animation graph unavailable"); return; }
    bodyRetry = std::max(0.0f, bodyRetry - dt);
    bool inJump{};
    player->GetGraphVariableBool("bInJumpState", inJump);
    if (flight.flying() && inJump && bodyRetry <= 0 && !player->IsAttacking()) {
        const bool accepted = player->NotifyAnimationGraph(RE::BSFixedString(usesVampireLordBehavior(player)?"JumpStandingEnd":"JumpLandEnd"));
        SKSE::log::info("Recovered native jump pose during flight: accepted={}", accepted);
        bodyRetry = 0.5f;
    }
    auto target = flight.velocity();
    if(flight.glideOnly()){
        target=observedVelocity;
        target.z=std::max(target.z,-config.tuning.glideFall);
    }
    const bool armed=player->AsActorState()->IsWeaponDrawn();
    const float requestedForward=controls.forward*(flight.highSpeed()?std::cos(controls.pitch):1.0f);
    const float requestedHorizontal=std::hypot(controls.right,requestedForward)*config.tuning.speed*
        flight.horizontalMultiplier(controls.sprint,config.tuning);
    const bool forwardFlight=wantsFlightFacing(true,flight.flying(),controls.forward,
        requestedHorizontal,armed,player->IsAttacking(),player->IsBlocking());
    flightBankTurn=forwardFlight && (armed || bodyMotion.forward()) && !flightFacing.targetLocked()?bodyTurn:0;
    const bool followFlight=config.faceFlight && forwardFlight;
    flightFacing.update(player,followFlight,requestedFlightYaw(controls.yaw,controls.right,requestedForward),dt,config.tuning.turnResponse);
    clearFall(controller);
    if(flight.flying())controller->context.currentState = RE::hkpCharacterStateTypes::kInAir;
    if(flight.flying())ownedGravity.apply(controller);else ownedGravity.release();
    controller->SetLinearVelocityImpl(RE::hkVector4(target.x, target.y, target.z, 0));
    if(flight.flying()&&config.collision.enabled&&config.collision.multiplier>0&&delta<=.1f)
        flightCollision.commit(controller,target.x,target.y,target.z);
    else flightCollision.reset();
    lastCell = player->GetParentCell(); lastPosition = position;
    lastWorld = player->GetWorldspace(); lastInterior = lastCell && lastCell->IsInteriorCell();
    sampleTimer += dt;
    if (config.logSamples && sampleTimer >= 2.0f) {
        sampleTimer = 0;
        SKSE::log::info("Flight motion: sprintHeld={} sprintActive={} flapMultiplier={:.3f} horizontalMultiplier={:.3f} hover={} hoverBaseZ={:.2f} hoverOffsetZ={:.2f} forwardPose={} glideRate={:.3f} facingOwned={} actorYaw={:.3f} cameraYaw={:.3f} armedLeanCOM={:.2f} flightBank={:.2f} pathPitch={:.2f} pitchOverlay={:.2f}",
            sprintHeld,controls.sprint,flight.speedMultiplier(),flight.horizontalMultiplier(controls.sprint,config.tuning),continuous,flight.hoverTarget()*RE::bhkWorld::GetWorldScaleInverse(),
            flight.hoverExcursion()*RE::bhkWorld::GetWorldScaleInverse(),bodyMotion.forward(),bodyMotion.glideSpeed(),flightFacing.active(),player->GetAngleZ(),controls.yaw,armedSprintLeanAngle(),flightBankAngle(),
            legs::flightPathPitch(target.x,target.y,target.z),flightPitchAngle());
        SKSE::log::info("Sample profile={} mode={} cmd=({:.2f},{:.2f},{:.2f}) measured=({:.2f},{:.2f},{:.2f}) supported={} flags={:08X} input=({:.2f},{:.2f}) fallStart={:.2f} playerZ={:.2f} health={:.1f}",
            lastProfile, static_cast<int>(flight.mode()), target.x, target.y, target.z,
            observedVelocity.x, observedVelocity.y, observedVelocity.z, supported(controller),
            static_cast<unsigned>(controller->flags.underlying()), controls.right, controls.forward,
            controller->fallStartHeight, position.z, player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
        bool graphFly{}, graphPose{};
        player->GetGraphVariableBool("Wingflight_IsFlying", graphFly);
        player->GetGraphVariableBool("Wingflight_UsePose", graphPose);
        SKSE::log::info("Animation sample: flying={} pose={} inJump={} armed={} attacking={} wingFound={} wingActive={} gliding={} phase={:.3f} rate={:.3f} requestedRate={:.3f} pendingStrokes={} stage={} sequences={}",
            graphFly, graphPose, inJump, player->AsActorState()->IsWeaponDrawn(), player->IsAttacking(),
            playback.found, playback.active, playback.gliding, playback.phase, playback.frequency, wingMotion.rate(),
            wingMotion.pending(), static_cast<int>(wingMotion.stroke()), playback.activeSequence);
        if(continuous)SKSE::log::info("Hover recovery: altitudeRequestIn={:.3f} phase={:.4f} rate={:.4f} pending={}",
            flight.automaticRecoverySeconds(timing.upstrokeSeconds,timing.downstrokeSeconds,config.tuning),playback.phase,wingMotion.rate(),wingMotion.pending());
    }
}
struct PlayerUpdate {
    static void thunk(RE::PlayerCharacter* actor, float delta) {
        nativeBatTravel.beginFrame(delta,ready&&loaded&&flight.flying()&&usesVampireLordBehavior(actor),nativeBatAnimationDriven(actor));
        if(nativeBatTravel.timedOut())SKSE::log::warn("Native Vampire Lord bat travel: no stop event within {:.1f}s; returning control to flight",vampire::BatTravel::limit);
        pauseVampireWingOverlay(nativeBatTravel.yielding());
        updateMenuPause(delta);
        if(ready&&loaded&&actorGraphSafe(actor)&&actor->GetCharController()){
            const auto match=wings.find(actor,flight.active());
            const auto controller=actor->GetCharController();
            updateHeldWings(match,delta,jumpHeld(),!supported(controller),flight.flying(),flight.glideOnly(),menuOpen(),config.glideHoldSeconds);
        }else resetHeldWings();
        if(ready&&loaded&&actorGraphSafe(actor))
            updateFlightBreath(actor,flight.flying()&&actor->GetCharController()==flightController
                &&flightController&&!supported(flightController));
        // Cancel immediately on physical support/replacement, before graph events.
        airborneExitEvents.update(actor,delta);
        // Disable native character acceleration before its update and retain a
        // reference for guaranteed restoration when a controller is replaced.
        if(ready&&loaded&&flight.flying()&&actorGraphSafe(actor)
            &&actor->GetCharController()==flightController)ownedGravity.apply(actor->GetCharController());
        else ownedGravity.release();
        setGlideClipGuard(actor,ready && loaded && flight.glideOnly() && jumpHeld()
            && actorGraphSafe(actor) && actor->GetCharController()==flightController
            && flightController && !supported(flightController));
        // Clear accumulated fall time before a landing is processed, but only while
        // this controller still owns a valid flight. No permanent perk or global setting.
        if (ready && loaded && flight.active()) {
            if (!actorGraphSafe(actor) || !actor->GetCharController() || actor->GetCharController() != flightController) {
                finish(actor, {}, "unsafe actor/controller before native update");
            } else if (usable(actor) && actor->GetRace() && actor->GetRace()->GetFormID() == flightRace) {
                const auto match = wings.find(actor,flight.active());
                if (match.profile) {
                    const auto controller = actor->GetCharController();
                    clearFall(controller);
                    // Once the engine reports supporting ground, stop pushing the
                    // controller back into InAir. Let the native landing update run.
                    if (supported(controller) && flight.readyToLand() && !nativeBatTravel.yielding()) {
                        const auto cell=actor->GetParentCell();
                        const bool continuousWorld=actor->GetWorldspace()==lastWorld
                            && !(lastCell!=cell&&(lastInterior||(cell&&cell->IsInteriorCell())))
                            && actor->GetPosition().GetDistance(lastPosition)<=4096.f;
                        const bool collided=flight.flying()&&!menuOpen()&&continuousWorld
                            &&flightCollision.update(actor,controller,delta,config.collision);
                        airInput.cancel(jumpHeld());
                        if(flight.flying()&&actorGraphSafe(actor)&&actor->GetCharController()==controller)
                            controller->SetLinearVelocityImpl(RE::hkVector4(0, 0, 0, 0));
                        finish(actor, match, collided?"damaging ground collision":"supporting ground before native update");
                    }
                }
            }
        }
        prepareArmedSprintIdle(actor,ready&&loaded&&config.armedFlightIdle&&flight.flying()&&!usesVampireLordBehavior(actor));
        prepareFlightPose(actor,ready&&loaded&&config.bodyTiming.enabled&&flight.flying()&&!usesVampireLordBehavior(actor));
        const auto flightVelocity=flight.velocity();
        prepareLowerBody(actor,ready&&loaded&&flight.flying(),delta,flight.highSpeed(),flightBankTurn,
            legs::flightPathPitch(flightVelocity.x,flightVelocity.y,flightVelocity.z),
            {{flightVelocity.x,flightVelocity.y,flightVelocity.z},config.tuning.gravity});
        original(actor, delta);
        const bool driven=nativeBatAnimationDriven(actor);
        const bool yielding=nativeBatTravel.afterNative(driven);
        pauseVampireWingOverlay(yielding);
        // Original graph/control updates prepared velocityMod from the actual
        // Bat clip. The native Air state then accelerates toward it too slowly.
        // Preserve the native request's direction/curve with the existing proxy
        // velocity setter; never move position, disable collisions or fake support.
        const auto controller=actorGraphSafe(actor)?actor->GetCharController():nullptr;
        const bool owned=ready&&loaded&&flight.flying()&&usesVampireLordBehavior(actor)&&controller&&controller==flightController;
        if(owned&&yielding&&driven&&std::isfinite(delta)&&delta>0&&!menuOpen()){
            const auto observed=velocity(controller);
            const auto& f=controller->forwardVec.quad.m128_f32;
            const auto& r=controller->velocityMod.quad.m128_f32;
            const auto target=vampire::batVelocity(owned,yielding,driven,supported(controller),
                {f[0],f[1],f[2]},{r[0],r[1],r[2]},{observed.x,observed.y,observed.z});
            if(target){
                controller->SetLinearVelocityImpl(RE::hkVector4((*target)[0],(*target)[1],(*target)[2],0));
                if(nativeBatVelocitySamples++<3)SKSE::log::info("Native Vampire Lord bat velocity: nativeRequest=({:.3f},{:.3f}) filtered=({:.3f},{:.3f}) applied=({:.3f},{:.3f}); native vertical/collision retained",
                    r[0],r[1],observed.x,observed.y,(*target)[0],(*target)[1]);
            }else if(!supported(controller))diagnostic::report("vampire.bat-velocity-invalid","retain native filtered velocity",controller,"forward/request/observed vectors");
        }else if(!yielding)nativeBatVelocitySamples=0;
        // Native update may enter/leave the Tween camera during this frame.
        updateMenuPause(0);
        update(actor, delta);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct PlayerGraphFilter {
    static bool thunk(RE::IAnimationGraphManagerHolder* holder, const RE::BSFixedString& event) {
        const auto owner=RE::PlayerCharacter::GetSingleton();
        const bool nativeTravelRequest=event=="BatSprintStart"&&owner&&holder==static_cast<RE::IAnimationGraphManagerHolder*>(owner)
            &&managedAirAnimation&&usesVampireLordBehavior(owner);
        if(nativeTravelRequest){nativeBatTravel.event("BatSprintStart");pauseVampireWingOverlay(true);}
        if(event.c_str()&&event=="SprintStart"){
            const auto player=RE::PlayerCharacter::GetSingleton();
            if(player&&holder==static_cast<RE::IAnimationGraphManagerHolder*>(player)
                &&suppressArmedSprintStart(armedSprintForbidden(player),event.c_str())){
                if(blockedSprintEvents.fetch_add(1)<16)SKSE::log::info("Armed sprint restriction consumed SprintStart; nativeSprint={}",player->AsActorState()->IsSprinting());
                return true;
            }
        }
        if (managedAirAnimation && (nativeJumpEvent(event.c_str())
            ||(usesVampireLordBehavior(RE::PlayerCharacter::GetSingleton())&&nativeVampireJumpEvent(event.c_str())))) {
            const auto player = RE::PlayerCharacter::GetSingleton();
            if (player && holder == static_cast<RE::IAnimationGraphManagerHolder*>(player)
                && actorGraphSafe(player) && player->GetCharController() && !supported(player->GetCharController())) {
                if (blockedBodyEvents.fetch_add(1) < 30) {
                    SKSE::log::info("Suppressed native airborne jump/landing request: {}", event.c_str());
                }
                return true;
            }
        }
        const bool accepted=original(holder, event);
        if(nativeTravelRequest&&!accepted)nativeBatTravel.event("batSprintStop");
        return accepted;
    }
    static inline REL::Relocation<decltype(thunk)> original;
    static void install() {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) { SKSE::log::error("Player graph guard unavailable: no player"); return; }
        const auto holder = static_cast<RE::IAnimationGraphManagerHolder*>(player);
        REL::Relocation<std::uintptr_t> table{*reinterpret_cast<std::uintptr_t*>(holder)};
        original = table.write_vfunc(1, thunk);
    }
};
struct ArmedIdleChannels {
    static void thunk(RE::IPostAnimationChannelUpdateFunctor* functor){
        original(functor);
        if(!ready)return;
        const auto player=RE::PlayerCharacter::GetSingleton();
        if(!player||functor!=player->AsIPostAnimationChannelUpdateFunctor())return;
        const auto controller=actorGraphSafe(player)?player->GetCharController():nullptr;
        const bool safe=controller&&!player->IsInKillMove();
        const bool wanted=armedIdleEligible(player),forbidden=armedSprintForbidden(player),nativeSprint=player->AsActorState()->IsSprinting();
        const auto change=armedSprintLatch.update(forbidden,nativeSprint,safe&&(managedAirAnimation||supported(controller)));
        if(change.exit&&safe){
            player->SetGraphVariableInt("iSyncSprintState",nativeSprint?1:0);
            // Resume the native animation only for continued full flight (sheathing)
            // or supported ground. C/freefall must keep its falling graph state.
            if(change.resumeSprint)player->NotifyAnimationGraph("SprintStart");
            SKSE::log::info("Armed sprint restriction released: nativeSprint={} animationResumed={}",nativeSprint,change.resumeSprint);
        }
        // The 0.2.10 graph exit also stopped native sprint on later frames.
        // Keep that behavior ONLY for the explicit gameplay restriction.
        // Normal cosmetic idle routing leaves the sprint state machine intact.
        if(forbidden)player->SetGraphVariableInt("iSyncSprintState",0);
        if(forbidden&&change.enter){
            const bool sent=player->NotifyAnimationGraph("SprintStop");
            SKSE::log::info("Armed sprint restriction stop requested: accepted={} nativeSprintBefore={} nativeSprintAfter={}",sent,nativeSprint,player->AsActorState()->IsSprinting());
        }
        if(unarmedPoseEligible(player)){
            float nativeSpeed{};
            if(player->GetGraphVariableFloat("Speed",nativeSpeed))
                player->SetGraphVariableInt("Wingflight_BodyPose",pose::graphMode(nativeSpeed,nativeSprint));
            // Keep the native idle/movement/sprint graph and its transition
            // weights. Only armed flight needs cosmetic locomotion suppression.
            return;
        }
        if(!wanted)return;
        float speed{},sampled{};
        const bool report=config.logSamples&&samples.load()<12;
        if(report){player->GetGraphVariableFloat("Speed",speed);player->GetGraphVariableFloat("SpeedSampled",sampled);}
        // Verified on 1.6.1170: native channels are copied into graph variables
        // BEFORE this callback, every update, and graph evaluation follows it.
        // Override only locomotion weights; retain native velocity, sprint flag,
        // direction, weapon state and attack events. The next native copy restores
        // normal values automatically when this gate no longer applies.
        const bool a=player->SetGraphVariableFloat("Speed",0.0f);
        const bool b=player->SetGraphVariableFloat("SpeedSampled",0.0f);
        if(report&&samples.fetch_add(1)<12)
            SKSE::log::info("Armed idle channels: Speed={:.3f}->0 written={} SpeedSampled={:.3f}->0 written={} nativeSprint={}",
                speed,a,sampled,b,player->AsActorState()->IsSprinting());
    }
    static inline std::atomic_uint samples{};
    static inline REL::Relocation<decltype(thunk)> original;
    static void install(){
        if(!config.armedFlightIdle&&!config.disableSprintWhenArmed&&!config.bodyTiming.enabled)return;
        const auto player=RE::PlayerCharacter::GetSingleton();if(!player)return;
        const auto functor=player->AsIPostAnimationChannelUpdateFunctor();
        REL::Relocation<std::uintptr_t> table{*reinterpret_cast<std::uintptr_t*>(functor)};
        original=table.write_vfunc(1,thunk);
        SKSE::log::info("Armed flight idle installed at post-channel/pre-evaluation callback; native movement and sprint remain authoritative");
    }
};
struct PlayerFootstepFilter {
    using Sink=RE::BSTEventSink<RE::BSAnimationGraphEvent>;
    using Source=RE::BSTEventSource<RE::BSAnimationGraphEvent>;
    static RE::BSEventNotifyControl thunk(Sink* sink,const RE::BSAnimationGraphEvent* event,Source* source){
        if(event){
            const auto player=RE::PlayerCharacter::GetSingleton();
            const bool ours=player&&event->holder==player&&sink==static_cast<Sink*>(player);
            if(ours&&managedAirAnimation&&usesVampireLordBehavior(player)&&event->tag.c_str()
                &&nativeBatTravel.event(event->tag.c_str()))pauseVampireWingOverlay(true);
            if(ours&&(muteAirFootEvent(event)||muteNativeWingEvent(event))){
                if(samples.fetch_add(1)<16)SKSE::log::info("Suppressed airborne footstep consumer: tag={} payload={}",event->tag.c_str(),event->payload.c_str()?event->payload.c_str():"");
                // Skip only this player's native footstep consumer. Continue event
                // dispatch for other sinks; never block attack or real landing tags.
                return RE::BSEventNotifyControl::kContinue;
            }
        }
        return original(sink,event,source);
    }
    static inline std::atomic_uint samples{};
    static inline REL::Relocation<decltype(thunk)> original;
    static void install(){
        const auto player=RE::PlayerCharacter::GetSingleton();if(!player)return;
        const auto sink=static_cast<Sink*>(player);
        REL::Relocation<std::uintptr_t> table{*reinterpret_cast<std::uintptr_t*>(sink)};
        original=table.write_vfunc(1,thunk);
        SKSE::log::info("Player footstep event filter installed; airborne foot tags/direct foot sounds only, physical support bypasses");
    }
};
struct JumpFilter {
    static bool thunk(RE::JumpHandler* handler, RE::InputEvent* event) {
        if (ready && loaded && !menuOpen() && event) {
            const auto button = event->AsButtonEvent();
            if (button && std::string_view(button->QUserEvent().c_str()) == "Jump") {
                if (flight.flying()) { return false; }
                const auto player = RE::PlayerCharacter::GetSingleton();
                const auto controller = player ? player->GetCharController() : nullptr;
                const bool airborne = controller && player->IsInMidair() && !supported(controller);
                // Claim the initiating air press BEFORE the native jump handler
                // can pass it to Better Jumping's jump/coyote hooks. Ground input
                // and an ineligible actor always keep the original handler chain.
                if (button->IsDown() && airborne && usable(player)) {
                    bool graph{};
                    const auto match = wings.find(player);
                    if (claimJumpForFlight(false, true, airborne, match.profile != nullptr, true,
                        player->GetGraphVariableBool("Wingflight_IsFlying", graph),
                        (match.profile&&match.profile->continuousFlight) || player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) >= config.tuning.flapCost)) {
                        std::lock_guard lock(inputMutex);
                        input.takeoff = true; input.flap = true;
                        SKSE::log::info("Claimed airborne Jump for flight before the native jump handler");
                        return false;
                    }
                }
            }
        }
        const bool accepted=original(handler,event);
        if(accepted&&ready&&loaded&&event){
            const auto b=event->AsButtonEvent();const auto p=RE::PlayerCharacter::GetSingleton();
            if(b&&b->IsDown()&&std::string_view(b->QUserEvent().c_str())=="Jump"&&usable(p)
                &&supported(p->GetCharController())&&wings.find(p).profile)groundJumpRequested=true;
        }
        return accepted;
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
struct SneakFilter {
    static bool thunk(RE::SneakHandler* handler, RE::InputEvent* event) {
        if (flight.active() && !menuOpen() && event) {
            if (const auto button = event->AsButtonEvent()) {
                const auto id = button->device==RE::INPUT_DEVICE::kGamepad
                    ?canonicalGamepadButton(button->GetIDCode()):button->GetIDCode();
                if ((button->device == RE::INPUT_DEVICE::kKeyboard
                        && ((config.descendKey != 0 && id == config.descendKey) || (config.landKey != 0 && id == config.landKey)))
                    || (button->device == RE::INPUT_DEVICE::kGamepad
                        && ((config.padDescend != 0 && id == config.padDescend) || (config.padLand != 0 && id == config.padLand)))) { return false; }
            }
        }
        return original(handler, event);
    }
    static inline REL::Relocation<decltype(thunk)> original;
};
void save(SKSE::SerializationInterface* serialization) {
    if(!writeFlightSave(*serialization,flight,preferredFlightMode))SKSE::log::warn("Unable to save Wingflight flight state/mode preference");
}
void load(SKSE::SerializationInterface* serialization) {
    const auto state=readFlightSave(*serialization);
    resumeMode=state.resumeMode;resumeWait=0;preferredFlightMode=state.preferredMode;
}
void resetSession() {
    jumpWingPreview.reset();jumpPreviewOwned=false;jumpPreviewEffect=nullptr;
    resetHeldWings();
    flightCollision.reset();
    resetDiagnosticLog();
    menuFlightPause.reset();tweenHandoffPending=false;flightMenuSuspended=false;
    nativeBatTravel.reset();nativeTravelWasPaused=false;pauseVampireWingOverlay(false);
    if(const auto player=RE::PlayerCharacter::GetSingleton();actorGraphSafe(player))updateFlightBreath(player,false);
    airborneExitEvents.reset();
    ownedGravity.release();
    resetAWUCompatibility();
    armedSprintLatch.reset();
    resetLowerBody();
    setGlideClipGuard(nullptr,false);
    prepareArmedSprintIdle(nullptr,false);
    prepareFlightPose(nullptr,false);
    managedAirAnimation = false; wings.clearVisual(); bodyRetry = 0;
    airInput.reset();jumpBoost.reset();groundJumpRequested=false;
    flightFacing.release();bodyMotion.reset(); flight.reset(); clearInput(); resetGraphs = true; resumeMode = -1; resumeWait = 0;
    preferredFlightMode=Mode::free;
    flightBankTurn=0;
    flightRace = 0; lastCell = nullptr; lastWorld = nullptr; lastInterior = false; lastEvent.clear(); lastProfile.clear();
    sampleTimer = wingRetry = ceilingTimer = 0; wingMotion.reset(); flightController = nullptr; visualEffect = nullptr;
}
void message(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
    case SKSE::MessagingInterface::kPostPostLoad:
        installFlightPoseLoader();break;
    case SKSE::MessagingInterface::kPreLoadGame:
        loaded = false; resetSession(); break;
    case SKSE::MessagingInterface::kNewGame:
        resetSession(); loaded = true; break;
    case SKSE::MessagingInterface::kPostLoadGame:
        loaded = message->data != nullptr; resetGraphs = true; break;
    case SKSE::MessagingInterface::kDataLoaded: {
        registerSettingsMenu(config,[](const Configuration& candidate)->std::string {
            if(const auto error=validateLiveSettings(candidate);!error.empty())return error;
            const auto controls=RE::ControlMap::GetSingleton();
            const auto events=RE::UserEvents::GetSingleton();
            if(controls&&events) {
                const auto jump=controls->GetMappedKey(events->jump,RE::INPUT_DEVICE::kKeyboard);
                const auto sprint=controls->GetMappedKey(events->sprint,RE::INPUT_DEVICE::kKeyboard);
                for(const auto key:{candidate.modeKey,candidate.landKey,candidate.descendKey})
                    if(key!=0&&(key==jump||key==sprint||key==1))
                        return "Mode, Exit Flight and Descend cannot use Jump, Sprint or Escape.";
                for(const auto& event:{events->jump,events->sprint,events->pause,events->tweenMenu}) {
                    const auto mapped=controls->GetMappedKey(event,RE::INPUT_DEVICE::kGamepad);
                    if(mapped==0xFF||mapped==0xFFFFFFFF)continue;
                    const auto button=canonicalGamepadButton(mapped);
                    for(const auto code:{candidate.padMode,candidate.padLand,candidate.padDescend})
                        if(code!=0&&code==button)return "Gamepad Mode, Exit Flight and Descend cannot use Jump, Sprint, Pause or Tween Menu.";
                }
            }
            const auto inverseScale=RE::bhkWorld::GetWorldScaleInverse();
            if(!std::isfinite(inverseScale)||inverseScale<=0)return "Physics world scale is not ready.";
            // The menu only submits a snapshot. Persistence and the complete
            // configuration commit run together on the SKSE game task thread.
            saveLiveSettings(std::filesystem::absolute("Data/SKSE/Plugins/Wingflight.ini"),candidate);
            applyLiveSettings(config,candidate,inverseScale);
            flightCollision.reset();
            applyLowerBodySettings(config);
            clearInput();
            airInput.cancel(false);groundJumpRequested=false;
            return {};
        });
        if (!config.enabled) { SKSE::log::info("Disabled by configuration"); break; }
        const auto data = RE::TESDataHandler::GetSingleton();
        if (GetModuleHandleW(L"FlightFramework.dll") || data->LookupLoadedModByName("P1FlyingRing.esp")
            || data->LookupLoadedLightModByName("P1FlyingRing.esp")) {
            SKSE::log::error("Flight disabled: another flight controller is installed (FlightFramework/P1FlyingRing)"); break;
        }
        flightFacing.initialize();
        initializeAWUCompatibility(config.awuCompatibility);
        wings.resolve(config);
        initializeFlightAudio(config.muteFlightSprintBreath);
        RE::BSInputDeviceManager::GetSingleton()->AddEventSink(&inputEvents);
        RE::UI::GetSingleton()->AddEventSink(&flightMenuEvents);
        REL::Relocation<std::uintptr_t> table{RE::VTABLE_PlayerCharacter[0]};
        PlayerUpdate::original = table.write_vfunc(0xAD, PlayerUpdate::thunk);
        REL::Relocation<std::uintptr_t> jump{RE::VTABLE_JumpHandler[0]};
        JumpFilter::original = jump.write_vfunc(1, JumpFilter::thunk);
        REL::Relocation<std::uintptr_t> sneak{RE::VTABLE_SneakHandler[0]};
        SneakFilter::original = sneak.write_vfunc(1, SneakFilter::thunk);
        PlayerGraphFilter::install();
        PlayerFootstepFilter::install();
        ArmedIdleChannels::install();
        initializeArmedSprintIdle(armedIdleEligible);
        initializeFlightPose(unarmedPoseEligible,[](RE::PlayerCharacter*){return bodyMotion.pose();});
        configureFlightPoseTransitions({config.movementBlendSeconds,config.sprintBlendInSeconds,config.sprintBlendOutSeconds});
        installGlideClipGuard();
        installLowerBody(config);
        if(!wings.initialize(config.spreadAmplitudeMultiplier)) {
            SKSE::log::error("Wing-art driver unavailable; AWU/MDDA flight disabled; native Vampire Lord backend readiness is independent");
            if(!vampireSceneReady())break;
        }
        SKSE::log::info("Integrated providers revision=vlrn-auto-7-se1597; Wingflight 0.3.14; vanilla/VLrn/AWU wing support and SE 1.5.97 runtime dispatch");
        installHeldWingEvents();
        SKSE::log::info("Better Jumping detected={}; ground/ineligible jumps preserve original handler; eligible air presses belong to Wingflight",
            GetModuleHandleW(L"BetterJumpingSE.dll") != nullptr);
        ready = true;
        SKSE::log::info("Ready. Armed idle={} DisableSprintWhenArmed={}. BFCO-independent. No attack, stamina-drain, collision-filter or gravity hooks. Physical game validation pending.",config.armedFlightIdle,config.disableSprintWhenArmed);
        break;
    }
    default: break;
    }
}
}

extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version = [] {
    SKSE::PluginVersionData version;
    version.PluginVersion({0, 3, 14, 0}); version.PluginName("Wingflight"); version.AuthorName("ROSVER");
    // The generic-struct flag covers CommonLib's version-selected accessors;
    // the post-629-only bit is rejected by older SKSE loaders.
    version.UsesAddressLibrary(); version.UsesNoStructs();
    static_assert(wingflight::runtime::supportedVersions.size()<std::size(version.compatibleVersions));
    for(std::size_t i=0;i<wingflight::runtime::supportedVersions.size();++i){const auto& v=wingflight::runtime::supportedVersions[i];
        version.compatibleVersions[i]=REL::Version(v.major,v.minor,v.build,v.revision).pack();}
    return version;
}();
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const SKSE::QueryInterface* skse,SKSE::PluginInfo* info) {
    if(!skse||!info)return false;
    info->infoVersion=SKSE::PluginInfo::kVersion;
    info->name="Wingflight";
    info->version=REL::Version(0,3,14,0).pack();
    const auto version=skse->RuntimeVersion();
    return !skse->IsEditor()&&runtime::supported(runtime::select(version[0],version[1],version[2],version[3]));
}
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    const auto host=skse->RuntimeVersion();
    if(!runtime::supported(runtime::select(host[0],host[1],host[2],host[3]))){
        // If SKSE calls Load, retain the same rejection but leave its exact cause.
        // SKSE may reject declared CompatibleVersions before reaching this code.
        try {
            if(auto directory=SKSE::log::log_directory()){
                auto sink=std::make_shared<spdlog::sinks::basic_file_sink_mt>((*directory/"Wingflight.log").string(),true);
                spdlog::set_default_logger(std::make_shared<spdlog::logger>("Wingflight",sink));spdlog::flush_on(spdlog::level::err);
            }
            SKSE::log::error("Wingflight AE16 test patch rejected runtime={}.{}.{}.{}; explicit Steam/GOG list only; Epic/Store/VR excluded",host[0],host[1],host[2],host[3]);
        }catch(...){}
        return false;
    }
    SKSE::Init(skse);
    if (auto directory = SKSE::log::log_directory()) {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*directory / "Wingflight.log").string(), true);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("Wingflight", sink));
        spdlog::set_level(spdlog::level::info); spdlog::flush_on(spdlog::level::info);
    }
    installDiagnosticLog();
    SKSE::log::info("Flight poses revision=dynamic-leg-poses-1: configurable native flight transitions; BodyPose=1/2/3; INI schema migration; single OAR pose group and FAR fallback");
    SKSE::log::info("Flight steering revision=reverse-brake-1: near-opposite movement brakes through zero; body facing follows requested flight direction");
    SKSE::log::info("Flight leg handoff revision=weapon-transition-1: draw/sheath and pose recovery retain continuous lower-body coverage");
    SKSE::log::info("AE16 compatibility TEST PATCH 1: runtime={} store={} structs={} bodyHook={}; older AE/GOG gameplay validation pending",
        runtimeName(),runtime::gog(runtimeBackend())?"GOG":"Steam",runtime::before629(runtimeBackend())?"pre-629":"post-629",
        runtime::bodyCallsite(runtimeBackend())?"verified call-site chain":"resolved scene-entry chain");
    SKSE::log::info("Wingflight failure diagnostics revision=failure-details-1; existing guard decisions unchanged; repeated causes limited by context");
    try { config = wingflight::loadAndMigrateConfiguration(); }
    catch (const std::exception& error) { SKSE::log::critical("Configuration error: {}", error.what()); return false; }
    SKSE::log::info("Sprint wings revision=configurable-recovery-1: HighSpeedFlapHoldSeconds={:.2f}; entry reuses downswing only above estimated 70 percent raised height",config.highSpeedFlapHoldSeconds);
    SKSE::log::info("Flight collision revision=unarmed-pain-3: damaging impact ends flight, unarmed flesh impact plus native player Hit topic; Enabled={} Multiplier={:.2f} DamageActors={} MinImpactSpeed={:.2f}",config.collision.enabled,config.collision.multiplier,config.collision.damageActors,config.collision.minSpeed);
    SKSE::log::info("AWU jump preview revision=absolute-over-open-5: open={:.2f}% initialOpen={:.2f}% opening={:.2f}s settle={:.2f}s flutterPeriod={:.2f}s flutterAmplitude={:.2f} (independent of SpreadAmplitudeMultiplier)",config.jumpWingOpenPercent,config.jumpWingOverOpenPercent,config.jumpWingOpenSeconds,config.jumpWingSettleSeconds,config.jumpWingFlutterPeriod,config.jumpWingFlutterAmplitude);
    const auto serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(flightStateRecord); serialization->SetSaveCallback(save); serialization->SetLoadCallback(load);
    serialization->SetRevertCallback([](SKSE::SerializationInterface*) { resetSession(); });
    SKSE::GetPapyrusInterface()->Register([](RE::BSScript::IVirtualMachine* vm) {
        vm->RegisterFunction("IsEnabled", "WingflightNative", +[](RE::StaticFunctionTag*) -> bool { return ready.load(); });
        return true;
    });
    SKSE::log::info("Wingflight 0.3.14 ALPHA for Skyrim {}. {} adapters. FAR resource defaults and optional OAR flight poses; integrated wing providers and runtime compatibility; optional Mod Control Panel. 1.7 gameplay validation pending.",runtimeName(),config.profiles.size());
    return SKSE::GetMessagingInterface()->RegisterListener(message);
}
