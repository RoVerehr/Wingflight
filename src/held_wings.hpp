#pragma once
#include "wings.hpp"
#include "wing_models.hpp"
namespace wingflight {
inline bool heldWingsEligible(const WingProfile* p){return nativeWingControls(p);}
void installHeldWingEvents();
bool heldWingGateInstalled();
void resetHeldWings();
void landHeldWings(float dt,bool restoreSneak=false);
void updateHeldWings(WingMatch match,float dt,bool held,bool airborne,bool fullFlight,bool glide,bool suspended,float threshold,bool enabled,bool jumpPending=false,bool foldSneaking=false,bool sneakPending=false);
}
