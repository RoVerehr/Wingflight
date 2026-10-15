#pragma once
#include "config.hpp"
#include <limits>
#include <RE/Skyrim.h>
namespace wingflight {
class Wings;
void initializeActorFlight(const Configuration&,Wings&);
void updateActorFlightLeader(RE::PlayerCharacter*,float,bool flying,bool paused,float hoverReference=std::numeric_limits<float>::quiet_NaN(),bool highSpeed=false);
void resetActorFlight();
bool actorFlightActive(const RE::Actor*);
// Animation callbacks consume cached ownership, not an AI/package query.
bool actorFlightPoseSelected(const RE::hkbCharacter*);
bool actorFlightPoseEligible(const RE::hkbCharacter*);
void prepareActorFlightPose(RE::Actor*,bool,int mode=1,bool falling=false);
bool actorFallClipGuard(const RE::hkbCharacter*);
int actorFlightPoseMode(const RE::hkbCharacter*);
RE::ActorHandle actorFlightPoseOwner(const RE::hkbCharacter*);
float actorContinuousWingRate(const RE::hkbClipGenerator*,const RE::hkbContext&);
}
