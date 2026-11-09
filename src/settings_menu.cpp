#include "settings_menu.hpp"
#include "live_settings.hpp"
#include "gamepad_input.hpp"
#include "diagnostics.hpp"
#include "registered_wings.hpp"
#include "wing_appearance.hpp"
#include "actor_flight.hpp"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <format>
#pragma warning(push)
#pragma warning(disable: 4996 4099 5054) // Unmodified third-party API header.
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace wingflight {
namespace {
std::mutex menuMutex;
Configuration draft;
ApplyMenuSettings applySettings;
ApplyWingAppearanceSettings applyAppearanceSettings;
appearance::ProfileSettings appearanceDraft;
std::string appearanceKey,appearanceStatus="Changes apply only to the wings currently equipped or summoned.";
std::uint64_t appearanceRevision{};
bool appearancePending{};
std::string status="Changes take effect after Apply and Save. No game restart is required.";
bool pending{};
int capture{}; // keyboard 1..3, gamepad 4..6; guarded by menuMutex
bool hotkeyWasEnabled{};
std::atomic_bool registered{};
std::unique_ptr<SKSEMenuFramework::Model::InputEvent> captureInput;
using MainWindow=SKSEMenuFramework::Model::WindowInterface* (*)();
MainWindow mainWindow{};

std::string keyName(std::uint32_t code) {
    if(code==0)return "Unbound";
    static constexpr std::pair<unsigned,const char*> names[]={
        {1,"Escape"},{2,"1"},{3,"2"},{4,"3"},{5,"4"},{6,"5"},{7,"6"},{8,"7"},{9,"8"},{10,"9"},{11,"0"},
        {12,"Minus"},{13,"Equals"},{14,"Backspace"},{15,"Tab"},{16,"Q"},{17,"W"},{18,"E"},{19,"R"},{20,"T"},
        {21,"Y"},{22,"U"},{23,"I"},{24,"O"},{25,"P"},{26,"Left Bracket"},{27,"Right Bracket"},{28,"Enter"},
        {29,"Left Ctrl"},{30,"A"},{31,"S"},{32,"D"},{33,"F"},{34,"G"},{35,"H"},{36,"J"},{37,"K"},{38,"L"},
        {39,"Semicolon"},{40,"Apostrophe"},{41,"Grave"},{42,"Left Shift"},{43,"Backslash"},{44,"Z"},{45,"X"},
        {46,"C"},{47,"V"},{48,"B"},{49,"N"},{50,"M"},{51,"Comma"},{52,"Period"},{53,"Slash"},{54,"Right Shift"},
        {56,"Left Alt"},{57,"Space"},{58,"Caps Lock"},{87,"F11"},{88,"F12"},{156,"Numpad Enter"},
        {157,"Right Ctrl"},{184,"Right Alt"},{199,"Home"},{200,"Up"},{201,"Page Up"},{203,"Left"},
        {205,"Right"},{207,"End"},{208,"Down"},{209,"Page Down"},{210,"Insert"},{211,"Delete"}};
    for(const auto& [scan,name]:names)if(code==scan)return name;
    if(code>=59&&code<=68)return std::format("F{}",code-58);
    return std::format("Scan code {}",code);
}
void endCapture() {
    capture=0;
    SKSEMenuFramework::SetHotkeyEnabled(hotkeyWasEnabled);
}
bool __stdcall onInput(RE::InputEvent* event) {
    std::lock_guard lock(menuMutex);
    if(!capture)return false;
    if(!settingsMenuOpen()){endCapture();return false;}
    if(const auto button=event?event->AsButtonEvent():nullptr;button&&button->IsDown()) {
        const bool keyboard=button->device==RE::INPUT_DEVICE::kKeyboard;
        const bool gamepad=button->device==RE::INPUT_DEVICE::kGamepad;
        const auto id=gamepad?canonicalGamepadButton(button->GetIDCode()):button->GetIDCode();
        if(keyboard&&id==1){endCapture();status="Binding cancelled.";return true;}
        if(capture<=3) {
            if(!keyboard||id>255)return true;
            if(capture==1)draft.modeKey=id;
            else if(capture==2)draft.landKey=id;
            else draft.descendKey=id;
        } else {
            if(!gamepad)return true;
            if(!validGamepadButton(id)||id==0){status="Unsupported gamepad button. Choose another button or cancel.";
                diagnostic::report("settings.gamepad-capture-unsupported","keep waiting for supported button",nullptr,
                    std::to_string(id),id);return true;}
            if(capture==4)draft.padMode=id;
            else if(capture==5)draft.padLand=id;
            else draft.padDescend=id;
        }
        endCapture();status="New binding selected. Use Apply and Save to activate it.";
    }
    return true;
}
void tooltip(const char* text) {
    if(ImGuiMCP::IsItemHovered())ImGuiMCP::SetTooltip("%s",text);
}
void actions() {
    ImGuiMCP::Separator();
    if(ImGuiMCP::Button("Apply and Save")) {
        const auto error=validateLiveSettings(draft);
        if(!error.empty()){
            status=error;diagnostic::report("settings.validation-failed","do not apply draft settings",nullptr,error);}
        else {
            roundLiveSettings(draft);
            pending=true;status="Applying settings...";
            const auto candidate=draft;
            SKSE::GetTaskInterface()->AddTask([candidate] {
                std::string error;
                try {error=applySettings(candidate);}
                catch(const std::exception& exception){error=exception.what();}
                std::lock_guard lock(menuMutex);
                pending=false;
                if(error.empty()) {
                    status="Applied and saved to Data/SKSE/Plugins/Wingflight/Settings.ini.";
                    SKSE::log::debug("Mod Control Panel: live settings applied and saved");
                } else {status="Settings were not applied: "+error;SKSE::log::warn("Mod Control Panel: {}",error);}
            });
        }
    }
    ImGuiMCP::TextWrapped("%s",status.c_str());
}
void renderSection(const char* section) {
    std::lock_guard lock(menuMutex);
    ImGuiMCP::TextWrapped("Adjust values here, then select Apply and Save. Settings are shared across characters.");
    ImGuiMCP::TextWrapped("Hover over a parameter for its meaning. Ctrl-click a slider to enter an exact value.");
    if(std::string_view(section)=="FreeFlight")
        ImGuiMCP::TextWrapped("Automatic beats add lift during the downstroke, then coast under Flight/Gravity. Hold Descend for automatic wingbeats targeting Flight/DescentSpeed; Space still gives a full manual beat. Manual mode uses the same descending beats without holding Descend. Release resumes hovering at the new altitude. High-speed flight ignores Descend and follows the camera. AutoFlapCost = 0 makes automatic beats free.");
    if(std::string_view(section)=="Flight")
        ImGuiMCP::TextWrapped("Fairy/Chaurus wings use continuous flight: hover at rest, hold Jump to rise, hold Descend to descend. K is ignored. Camera direction controls high-speed flight. Descent uses DescentSpeed; outside-flight held-Jump glide uses the separate GlideFallSpeed. RiseSpeed is in AWU Fairy.ini and requires a restart. Wingbeat impulse/cost settings do not apply.");
    ImGuiMCP::BeginDisabled(pending||capture!=0);
    auto slider=[](const LiveFloatSetting& setting){
        ImGuiMCP::SliderFloat(setting.key,&setting.value(draft),setting.minimum,setting.maximum,"%.2f");
        tooltip(setting.help);
    };
    // Draw only the parameter for the selected ground behavior; keep both values.
    const auto groundHover=std::find_if(liveFloatSettings.begin(),liveFloatSettings.end(),
        [](const auto& setting){return std::string_view(setting.key)=="GroundHoverHeight";});
    const auto groundSpeed=std::find_if(liveFloatSettings.begin(),liveFloatSettings.end(),
        [](const auto& setting){return std::string_view(setting.key)=="GroundWingIdleSpeedPercent";});
    const auto groundBeat=std::find_if(liveFloatSettings.begin(),liveFloatSettings.end(),
        [](const auto& setting){return std::string_view(setting.key)=="GroundBeatDistance";});
    for(const auto& setting:liveFloatSettings)if(std::string_view(setting.section)==section
        &&&setting!=&*groundHover&&&setting!=&*groundSpeed&&&setting!=&*groundBeat)slider(setting);
    if(std::string_view(section)=="Flight") {
        ImGuiMCP::Checkbox("Allow Flight While Overencumbered",&draft.tuning.allowOverencumberedFlight);
        tooltip("Off by default: Skyrim's native overencumbrance check blocks player takeoff and held-Jump glide. Becoming overencumbered during either ends it normally. On permits flight regardless of carried weight. Does not change ordinary jumping, wing animations or NPC flight. Apply and Save takes effect immediately.");
        ImGuiMCP::Checkbox("End Flight on Ground",&draft.tuning.endFlightOnGround);
        tooltip("On: supporting ground ends full flight. Off: retain flight and use Ground Contact Behavior below. Held-Jump glide, damaging collisions and other flight interruptions keep their normal exit rules.");
        ImGuiMCP::BeginDisabled(draft.tuning.endFlightOnGround);
        int groundMode=draft.tuning.groundWingIdle?1:0;
        const char* groundChoices[]={"Normal flight (keep automatic lift)","Ground wing idle (Space to take off)"};
        if(ImGuiMCP::Combo("Ground Contact Behavior",&groundMode,groundChoices,2))draft.tuning.groundWingIdle=groundMode==1;
        tooltip("Only with End Flight on Ground off. Normal flight uses low ground beats in both K modes, up to GroundHoverHeight. Within GroundBeatDistance of the floor below, beats keep the normal cadence; farther away, beat speed adjusts itself so you sink at about DescentSpeed. Space leaves this floor cycle without changing the selected K mode. Ground wing idle keeps visual beats without automatic lift or beat cost; Space still lifts off. Apply and Save switches live.");
        if(draft.tuning.groundWingIdle){
            ImGuiMCP::SliderFloat("Ground wing idle speed (%)",&groundSpeed->value(draft),groundSpeed->minimum,groundSpeed->maximum,"%.0f%%");
            tooltip(groundSpeed->help);
        }else{
            slider(*groundHover);slider(*groundBeat);}
        ImGuiMCP::EndDisabled();
        ImGuiMCP::Checkbox("DisableSprintWhenArmed",&draft.disableSprintWhenArmed);
        tooltip("Disable armed sprint flight. Drawing a weapon also ends high-speed flight. Ground sprint is unchanged.");
    }
    if(std::string_view(section)=="Jump") {
        ImGuiMCP::Checkbox("Fold Wings While Sneaking",&draft.foldWingsWhileSneaking);
        tooltip("Keep player AWU dragon/feather wings folded while sneaking on the ground, in first or third person. Independent of Control Jump Wing Animations. Jumping, gliding, full flight and swimming take priority. Default off. Apply and Save takes effect immediately.");
        ImGuiMCP::Checkbox("Control Jump Wing Animations",&draft.controlJumpWingAnimations);
        tooltip("Control partial jump opening and require held Jump for AWU dragon/feather airborne flutter, including falling after flight exit or stepping off a ledge. While swimming, block only wing jump-opening events; normal ascent and swimming animations stay native. Sneaking, weapon draw and casting stay native. Off restores native jump/fall wing behavior. Full flight and the separate Glide/Enabled setting are unchanged. Apply and Save takes effect immediately.");
    }
    if(std::string_view(section)=="General") {
        ImGuiMCP::Checkbox("Detailed Logging",&draft.logSamples);
        tooltip("Off keeps initialization, compatibility checks, warnings and errors. On also records flight, wing, animation, NPC and combat details in Wingflight.log. Apply and Save takes effect immediately.");
        ImGuiMCP::TextWrapped("Leave off for normal play. Enable before reproducing a problem, then turn it off after collecting the log. Warnings and errors are always recorded.");
    }
    if(std::string_view(section)=="NPCFlight") {
        ImGuiMCP::Checkbox("Allow NPC Flight",&draft.actors.enabled);
        tooltip("Allow winged NPCs following the player to take off. Off prevents new takeoffs and safely lands airborne NPCs. Does not change player flight.");
        ImGuiMCP::Checkbox("Allow Air Combat",&draft.actors.airCombat);
        tooltip("Allow already-flying followers to fight in air. Native AI chooses enemies, equipment and attacks; Wingflight moves melee fighters into reach and keeps ranged fighters at weapon range. Off safely lands them for combat. Fleeing or unavailable combat data falls back to ground combat.");
        ImGuiMCP::Checkbox("Give Wings to Wingless Followers",&draft.actors.autoWings);
        tooltip("While you are in full flight, temporarily give following NPCs the selected AWU wings if they have no supported wings. Requires Animated Wings Ultimate and its wing providers. Existing wings take priority. No equipment or permanent abilities are added. When you stop flying or disable this option, followers land before the temporary wings are removed. Default off.");
        const auto wingOptions=followerWingOptions();
        if(wingOptions.empty())ImGuiMCP::TextWrapped("Temporary wings require Animated Wings Ultimate and its installed wing providers.");
        else{
            int selected=-1;std::vector<const char*> labels;
            for(std::size_t i=0;i<wingOptions.size();++i){labels.push_back(wingOptions[i].name.c_str());if(wingOptions[i].effectID==draft.actors.autoWingsEffect)selected=static_cast<int>(i);}
            ImGuiMCP::BeginDisabled(!draft.actors.enabled||!draft.actors.autoWings);
            if(ImGuiMCP::Combo("AWU wing style",&selected,labels.data(),static_cast<int>(labels.size()))&&selected>=0)
                draft.actors.autoWingsEffect=wingOptions[selected].effectID;
            tooltip("Choose from installed, recognized AWU wing styles. After Apply and Save, newly created pairs use this style. Followers already in flight keep their current wings until landing.");
            ImGuiMCP::EndDisabled();
            if(selected<0)ImGuiMCP::TextWrapped("The saved wing style is unavailable. Select an installed style to use automatic wings.");
        }
        ImGuiMCP::TextWrapped("FollowDistance sets the gap behind your current position. Apply and Save changes these settings without restarting. NPCs need supported wings (or the temporary AWU wings above) and an active follow order. Air combat ends in following or landing when combat stops.");
    }
    if(std::string_view(section)=="LowerBody") {
        ImGuiMCP::Checkbox("RelaxedArmedLegs",&draft.legRelaxation.enabled);
        tooltip("Armed full flight only. Off restores the previous fixed-leg overlay; unarmed poses are unchanged.");
        ImGuiMCP::Checkbox("SecondaryMotion",&draft.legRelaxation.secondaryMotion);
        tooltip("Enable damped calf/ankle inertia. Off keeps the selected thigh animation retention without procedural swing.");
        ImGuiMCP::TextWrapped("Apply and Save changes these values live. Parameter changes and swing toggles blend smoothly. LowerBody/Enabled remains an INI-only startup switch.");
    }
    if(std::string_view(section)=="Collision") {
        ImGuiMCP::Checkbox("Enabled",&draft.collision.enabled);
        tooltip("Enable physical collision damage during flight. One contact episode cannot drain health repeatedly. Native falling after exiting flight is unchanged.");
        ImGuiMCP::Checkbox("DamageActors",&draft.collision.damageActors);
        tooltip("Also damage directly contacted NPCs/creatures, including allies. The player remains vulnerable when this option is off.");
        ImGuiMCP::TextWrapped("Uses actual physics contacts, not nearby actors. Closing speed into a surface determines damage. Menus and bat travel suspend detection. Apply and Save changes these options live.");
    }
    actions();
    ImGuiMCP::EndDisabled();
}
void __stdcall renderAppearance(){
    const auto current=currentWingAppearance();
    std::lock_guard lock(menuMutex);
    if(current.key!=appearanceKey||current.revision!=appearanceRevision){
        appearanceKey=current.key;appearanceRevision=current.revision;appearanceDraft=current.values;
    }
    ImGuiMCP::TextWrapped("Current wings: %s",current.key.empty()?"None - equip or summon supported wings":current.name.c_str());
    ImGuiMCP::TextWrapped("Each pair of wings remembers its own settings. Equip different wings to edit them here. Apply and Save changes only the wings shown above.");
    if(!current.error.empty())ImGuiMCP::TextWrapped("%s",current.error.c_str());
    ImGuiMCP::BeginDisabled(current.key.empty()||appearancePending||pending||capture!=0);
    ImGuiMCP::SliderFloat("Normal size (%)",&appearanceDraft.normalScale,10,200,"%.2f%%");
    tooltip("Wing size outside Wingflight flight/glide. 100% is the model's original size. Ordinary jumps use this size.");
    ImGuiMCP::SliderFloat("Flying size (%)",&appearanceDraft.flightScale,10,200,"%.2f%%");
    tooltip("Size during full flight or held-Jump glide, including retained ground flight. Entering and exiting blends over about half a second; paused game time does not advance the blend.");
    ImGuiMCP::SliderFloat("Vertical offset",&appearanceDraft.verticalOffset,-60,60,"%.2f");
    tooltip("Move this pair of wings along the torso in Skyrim units. Negative moves toward the waist. The same height is used in both size modes.");
    if(ImGuiMCP::Button("Restore original size and position"))appearanceDraft={};
    ImGuiMCP::Separator();
    if(ImGuiMCP::Button("Apply and Save")){
        appearancePending=true;appearanceStatus="Saving current wing settings...";
        const auto key=appearanceKey;const auto values=appearanceDraft;
        SKSE::GetTaskInterface()->AddTask([key,values]{
            std::string error;try{error=applyAppearanceSettings(key,values);}catch(const std::exception& e){error=e.what();}
            std::lock_guard lock(menuMutex);appearancePending=false;
            appearanceStatus=error.empty()?"Saved for these wings. Other wings keep their own settings.":error;
        });
    }
    ImGuiMCP::EndDisabled();ImGuiMCP::TextWrapped("%s",appearanceStatus.c_str());
}
void __stdcall renderFlight(){renderSection("Flight");}
void __stdcall renderFree(){renderSection("FreeFlight");}
void __stdcall renderStamina(){renderSection("Stamina");}
void __stdcall renderJump(){renderSection("Jump");}
void __stdcall renderLowerBody(){renderSection("LowerBody");}
void __stdcall renderCollision(){renderSection("Collision");}
void __stdcall renderNPCFlight(){renderSection("NPCFlight");}
void __stdcall renderLogging(){renderSection("General");}
void binding(const char* label,int target,std::uint32_t& code) {
    ImGuiMCP::PushID(label);
    ImGuiMCP::Text("%s: %s",label,target>3?gamepadButtonName(code):keyName(code).c_str());
    ImGuiMCP::SameLine();
    if(ImGuiMCP::Button("Change")) {
        hotkeyWasEnabled=SKSEMenuFramework::IsHotkeyEnabled();
        SKSEMenuFramework::SetHotkeyEnabled(false);capture=target;
        status=target>3?"Press the gamepad button. Escape or Cancel Binding cancels.":"Press the keyboard key. Escape cancels.";
    }
    ImGuiMCP::SameLine();
    if(ImGuiMCP::Button("Unbind"))code=0;
    ImGuiMCP::PopID();
}
void __stdcall renderControls() {
    std::lock_guard lock(menuMutex);
    ImGuiMCP::TextWrapped("Keyboard bindings. Jump and Sprint follow Skyrim's own control mapping.");
    ImGuiMCP::TextWrapped("Exit Flight releases the flight controller into native falling. This is not a slow landing command.");
    ImGuiMCP::BeginDisabled(pending||capture!=0);
    binding("Cycle Mode",1,draft.modeKey);
    binding("Exit Flight",2,draft.landKey);
    binding("Descend",3,draft.descendKey);
    if(ImGuiMCP::Button("Default K / C / Ctrl")){draft.modeKey=37;draft.landKey=46;draft.descendKey=29;}
    ImGuiMCP::Separator();
    ImGuiMCP::TextWrapped("Gamepad bindings. Jump and Sprint follow Skyrim's own control mapping. Custom buttons apply immediately after Apply and Save.");
    binding("Gamepad Cycle Mode",4,draft.padMode);
    binding("Gamepad Exit Flight",5,draft.padLand);
    binding("Gamepad Descend",6,draft.padDescend);
    if(ImGuiMCP::Button("Unbind Gamepad Controls")){draft.padMode=draft.padLand=draft.padDescend=0;}
    actions();
    ImGuiMCP::EndDisabled();
    if(capture) {
        ImGuiMCP::TextWrapped("%s",capture>3?"Waiting for a gamepad button... Escape or Cancel Binding cancels.":"Waiting for a keyboard key... Escape cancels.");
        if(ImGuiMCP::Button("Cancel Binding")){endCapture();status="Binding cancelled.";}
    }
}
}
bool settingsMenuOpen() {
    if(!registered.load(std::memory_order_acquire)||!mainWindow)return false;
    const auto window=mainWindow();return window&&window->IsOpen.load();
}
void registerSettingsMenu(const Configuration& initial,ApplyMenuSettings apply,ApplyWingAppearanceSettings appearance) {
    // No import-library link: without this optional framework the INI still works.
    const auto module=GetModuleHandleW(L"SKSEMenuFramework.dll");
    if(!module){SKSE::log::info("SKSE Menu Framework is not loaded; Wingflight uses INI settings");return;}
    const char* required[]={"AddSectionItem","GetMainWindow","RegisterInpoutEvent","UnregisterInputEvent",
        "SetHotkeyEnabled","IsHotkeyEnabled","igSliderFloat","igCheckbox","igButton","igTextV","igTextWrappedV",
        "igSameLine","igSeparator","igBeginDisabled","igEndDisabled","igIsItemHovered","igSetTooltipV","igPushID_Str","igPopID"};
    for(const auto name:required)if(!GetProcAddress(module,name)) {
        SKSE::log::warn("SKSE Menu Framework lacks {}; menu skipped, INI remains available",name);return;
    }
    mainWindow=reinterpret_cast<MainWindow>(GetProcAddress(module,"GetMainWindow"));
    draft=initial;applySettings=std::move(apply);applyAppearanceSettings=std::move(appearance);
    SKSEMenuFramework::SetSection("Wingflight");
    SKSEMenuFramework::AddSectionItem("Flight",renderFlight);
    SKSEMenuFramework::AddSectionItem("NPC Flight",renderNPCFlight);
    SKSEMenuFramework::AddSectionItem("Wing Appearance",renderAppearance);
    bool customMenu=true;
    for(const auto name:{"igTextUnformatted","igNewLine","igPushID_Int"})if(!GetProcAddress(module,name)){
        customMenu=false;SKSE::log::warn("Custom Wings panel unavailable: SKSE Menu Framework lacks {}; registration INIs remain available",name);}
    if(customMenu)SKSEMenuFramework::AddSectionItem("Custom Wings",renderWingRegistrationMenu);
    SKSEMenuFramework::AddSectionItem("Free Flight",renderFree);
    SKSEMenuFramework::AddSectionItem("Stamina",renderStamina);
    SKSEMenuFramework::AddSectionItem("Jump",renderJump);
    SKSEMenuFramework::AddSectionItem("Lower Body",renderLowerBody);
    SKSEMenuFramework::AddSectionItem("Collision",renderCollision);
    SKSEMenuFramework::AddSectionItem("Controls",renderControls);
    captureInput=std::make_unique<SKSEMenuFramework::Model::InputEvent>(onInput);
    registered.store(true,std::memory_order_release);
    SKSEMenuFramework::AddSectionItem("Logging",renderLogging);
    SKSE::log::info("Registered Wingflight Mod Control Panel: Flight, NPC Flight, Wing Appearance, Custom Wings available={}, Free Flight, Stamina, Jump, Lower Body, Collision, Controls, Logging",customMenu);
}
}
