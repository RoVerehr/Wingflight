#pragma once
#include "config.hpp"
#include <filesystem>
#include <string>
#include <string_view>

namespace wingflight {
// Preserve comments, unrelated sections, custom adapters and line endings.
std::string updateLiveSettingsText(std::string_view original, Configuration settings);
void saveLiveSettings(const std::filesystem::path& path, const Configuration& settings);
// Canonical current layout; keep recognized values and migrate renamed keys.
std::string migrateSettingsText(std::string_view original);
Configuration loadAndMigrateConfiguration();
}
