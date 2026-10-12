#pragma once
#include "config.hpp"
#include <functional>
#include <string>

namespace wingflight {
using ApplyMenuSettings=std::function<std::string(const Configuration&)>;
void registerSettingsMenu(const Configuration& initial,ApplyMenuSettings apply);
bool settingsMenuOpen();
}
