#include "settings_menu.hpp"
#include "live_settings.hpp"
#include "gamepad_input.hpp"
#include "diagnostics.hpp"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
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
                    status="Applied and saved to Data/SKSE/Plugins/Wingflight.ini.";
                    SKSE::log::info("Mod Control Panel: live settings applied and saved");
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
        ImGuiMCP::TextWrapped("Automatic beats add lift during the downstroke, then coast under Flight/Gravity. Hold Descend for spread-wing slow descent; Space can still climb. Release resumes hovering at the new altitude. High-speed flight ignores Descend and follows the camera. AutoFlapCost = 0 makes automatic beats free.");
    if(std::string_view(section)=="Flight")
        ImGuiMCP::TextWrapped("Fairy/Chaurus wings use continuous flight: hover at rest, hold Jump to rise, hold Descend to descend. K is ignored. Camera direction controls high-speed flight. Descent uses GlideFallSpeed; RiseSpeed is in AWU Fairy.ini and requires a restart. Wingbeat impulse/cost settings do not apply.");
    ImGuiMCP::BeginDisabled(pending||capture!=0);
    for(const auto& setting:liveFloatSettings)if(std::string_view(setting.section)==section) {
        ImGuiMCP::SliderFloat(setting.key,&setting.value(draft),setting.minimum,setting.maximum,"%.2f");
        tooltip(setting.help);
    }
    if(std::string_view(section)=="Flight") {
        ImGuiMCP::Checkbox("DisableSprintWhenArmed",&draft.disableSprintWhenArmed);
        tooltip("Disable armed sprint flight. Drawing a weapon also ends high-speed flight. Ground sprint is unchanged.");
    }
    if(std::string_view(section)=="LowerBody") {
        ImGuiMCP::Checkbox("RelaxedArmedLegs",&draft.legRelaxation.enabled);
        tooltip("Armed full flight only. Off restores the previous fixed-leg overlay; unarmed poses are unchanged.");
        ImGuiMCP::Checkbox("SecondaryMotion",&draft.legRelaxation.secondaryMotion);
        tooltip("Enable damped calf/ankle inertia. Off keeps the selected thigh animation retention without procedural swing.");
        ImGuiMCP::TextWrapped("Apply and Save changes these values live. Parameter changes and swing toggles blend smoothly. LowerBody/Enabled remains an INI-only startup switch.");
    }
    actions();
    ImGuiMCP::EndDisabled();
}
void __stdcall renderFlight(){renderSection("Flight");}
void __stdcall renderFree(){renderSection("FreeFlight");}
void __stdcall renderStamina(){renderSection("Stamina");}
void __stdcall renderJump(){renderSection("Jump");}
void __stdcall renderLowerBody(){renderSection("LowerBody");}
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
void registerSettingsMenu(const Configuration& initial,ApplyMenuSettings apply) {
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
    draft=initial;applySettings=std::move(apply);
    SKSEMenuFramework::SetSection("Wingflight");
    SKSEMenuFramework::AddSectionItem("Flight",renderFlight);
    SKSEMenuFramework::AddSectionItem("Free Flight",renderFree);
    SKSEMenuFramework::AddSectionItem("Stamina",renderStamina);
    SKSEMenuFramework::AddSectionItem("Jump",renderJump);
    SKSEMenuFramework::AddSectionItem("Lower Body",renderLowerBody);
    SKSEMenuFramework::AddSectionItem("Controls",renderControls);
    captureInput=std::make_unique<SKSEMenuFramework::Model::InputEvent>(onInput);
    registered.store(true,std::memory_order_release);
    SKSE::log::info("Registered Wingflight Mod Control Panel: Flight, Free Flight, Stamina, Jump, Lower Body, Controls");
}
}
