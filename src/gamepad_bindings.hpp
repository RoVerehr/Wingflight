#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace wingflight {
struct GamepadButton { std::uint32_t code; const char* name; };
// Canonical XInput masks, plus Skyrim's two trigger IDs. Not SKSE macro codes.
inline constexpr auto gamepadButtons=std::to_array<GamepadButton>({
    {1,"D-pad Up"},{2,"D-pad Down"},{4,"D-pad Left"},{8,"D-pad Right"},
    {16,"Start / Options"},{32,"Back / Touchpad"},{64,"Left Stick / L3"},{128,"Right Stick / R3"},
    {256,"LB / L1"},{512,"RB / R1"},{4096,"A / Cross"},{8192,"B / Circle"},
    {16384,"X / Square"},{32768,"Y / Triangle"},{9,"LT / L2"},{10,"RT / R2"}
});
constexpr const char* gamepadButtonName(std::uint32_t code) {
    if(code==0)return "Unbound";
    for(const auto& button:gamepadButtons)if(button.code==code)return button.name;
    return "Unknown button";
}
constexpr bool validGamepadButton(std::uint32_t code) {
    if(code==0)return true;
    for(const auto& button:gamepadButtons)if(button.code==code)return true;
    return false;
}
inline std::string validateGamepadBindings(const std::array<std::uint32_t,3>& buttons) {
    for(auto code:buttons)if(!validGamepadButton(code))return "Gamepad bindings must use a single supported button, or 0 for Unbound.";
    for(std::size_t i=0;i<buttons.size();++i)
        for(std::size_t j=i+1;j<buttons.size();++j)
            if(buttons[i]!=0&&buttons[i]==buttons[j])return "Gamepad CycleMode, Land and Descend must use different buttons.";
    return {};
}
}
