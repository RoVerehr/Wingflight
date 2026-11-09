#pragma once
#include "wing_appearance_policy.hpp"
#include <RE/Skyrim.h>
#include <vector>

namespace wingflight {
struct WingMatch;
struct WingAppearanceRoots {
    RE::NiPointer<RE::NiAVObject> model;
    std::vector<RE::NiPointer<RE::NiAVObject>> roots;
    bool smp{};
};
WingAppearanceRoots registeredWingAppearanceRoots(const WingMatch& match);
struct WingAppearanceSnapshot {
    std::string key,name,error;
    appearance::ProfileSettings values;
    std::uint64_t revision{};
};
WingAppearanceSnapshot currentWingAppearance();
std::string saveCurrentWingAppearance(RE::Actor* actor,const WingMatch& match,const std::string& expectedKey,appearance::ProfileSettings values);
void updateWingAppearance(RE::Actor* actor,const WingMatch& match,const appearance::Settings& defaults,bool flying,float delta);
void resetWingAppearance();
// Remove our scale/translation before animation evaluation, then reapply after
// it. Rotation remains owned by native animation/the existing flight drivers.
void beginWingAppearance(RE::NiAVObject* scope);
void endWingAppearance(RE::NiAVObject* scope,RE::NiUpdateData* data=nullptr);
struct WingAppearancePass {
    RE::NiAVObject* scope{};
    RE::NiUpdateData* data{};
    explicit WingAppearancePass(RE::NiAVObject* object,RE::NiUpdateData* update=nullptr):scope(object),data(update){beginWingAppearance(scope);}
    ~WingAppearancePass(){endWingAppearance(scope,data);}
};
}
