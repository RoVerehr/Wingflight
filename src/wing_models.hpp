#pragma once
#include "config.hpp"
#include <RE/Skyrim.h>
#include <vector>
namespace wingflight {
struct WingMatch;
// Copies owning references before callers inspect graphs or send events.
std::vector<RE::NiPointer<RE::ModelReferenceEffect>> wingModels(const WingMatch& match);
void resetWingModelCache();
inline bool nativeWingControls(const WingProfile* p){
    return p&&(p->awuCompatibility||!p->artObjectIDs.empty())&&!p->nativeVampire
        &&!p->continuousFlight&&!p->animationless&&p->wingTemplate==channel::Template::legacy;
}
inline const char* wingFoldEvent(const WingProfile* p){return p&&p->awuCompatibility?"MTState":"JumpLandEnd";}
}
