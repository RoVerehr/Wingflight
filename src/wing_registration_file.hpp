#pragma once
#include "config.hpp"
#include <filesystem>
namespace wingflight {
void defaultWingRegistrationSounds(WingProfile& profile,bool awuAvailable);
WingProfile readWingRegistration(const std::filesystem::path& path);
std::string wingRegistrationText(const WingProfile& profile);
}
