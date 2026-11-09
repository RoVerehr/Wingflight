#pragma once
#include "wing_appearance_policy.hpp"
#include <filesystem>
#include <map>
namespace wingflight::appearance {
using Profiles=std::map<std::string,ProfileSettings>;
Profiles parseProfiles(std::string_view text);
std::string updateProfileText(std::string_view original,const std::string& key,ProfileSettings values);
Profiles loadProfiles(const std::filesystem::path& file);
Profiles saveProfile(const std::filesystem::path& file,const std::string& key,ProfileSettings values);
}
