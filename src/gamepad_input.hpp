#pragma once
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace wingflight {
inline std::uint32_t canonicalGamepadButton(std::uint32_t code) {
    const auto controls=RE::ControlMap::GetSingleton();
    if(controls&&controls->GetGamePadType()==RE::PC_GAMEPAD_TYPE::kOrbis)
        return SKSE::InputMap::ScePadOffsetToXInput(code);
    return code;
}
}
