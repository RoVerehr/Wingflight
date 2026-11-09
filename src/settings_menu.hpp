#pragma once
#include "config.hpp"
#include <functional>
#include <string>

namespace wingflight {
using ApplyMenuSettings=std::function<std::string(const Configuration&)>;
using ApplyWingAppearanceSettings=std::function<std::string(const std::string&,appearance::ProfileSettings)>;
void registerSettingsMenu(const Configuration& initial,ApplyMenuSettings apply,ApplyWingAppearanceSettings appearance);
bool settingsMenuOpen();
}
