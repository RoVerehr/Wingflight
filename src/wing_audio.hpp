#pragma once
#include <RE/Skyrim.h>
#include <string>
#include <vector>
namespace wingflight {
// File lists take precedence; an empty list preserves descriptor-based playback.
bool buildWingSound(RE::BSSoundHandle& handle,RE::BGSSoundDescriptorForm* descriptor,
    const std::vector<std::string>& files,std::uint32_t outputModel=0x1A2FF);
}
