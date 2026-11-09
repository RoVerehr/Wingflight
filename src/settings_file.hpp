#pragma once
#include "config.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace wingflight {
// Preserve comments, unrelated sections, custom adapters and line endings.
std::string updateLiveSettingsText(std::string_view original, Configuration settings);
void saveLiveSettings(const std::filesystem::path& path, const Configuration& settings);
// Runtime settings live outside the shipped INI. New files follow MO2's output
// routing (normally Overwrite); never ship this generated player file.
std::filesystem::path userSettingsPath();
void saveUserSettings(const Configuration& settings);
// Canonical current layout; keep recognized values and migrate renamed keys.
std::string migrateSettingsText(std::string_view original);
// Import the installed/legacy main INI once, then migrate only the user file.
Configuration loadAndMigrateConfiguration();
}
