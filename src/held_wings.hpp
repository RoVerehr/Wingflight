#pragma once
#include "wings.hpp"
namespace wingflight {
inline bool heldWingsEligible(const WingProfile* p){return p&&p->awuCompatibility&&!p->nativeVampire&&!p->continuousFlight&&!p->animationless;}
void installHeldWingEvents();
bool heldWingGateInstalled();
void resetHeldWings();
void updateHeldWings(WingMatch match,float dt,bool held,bool airborne,bool fullFlight,bool glide,bool suspended,float threshold);
}
