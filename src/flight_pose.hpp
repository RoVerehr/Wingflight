#pragma once
#include <RE/Skyrim.h>
#include "flight_transition_policy.hpp"
namespace wingflight {
using FlightPoseGate=bool(*)(RE::PlayerCharacter*);
using FlightPoseMode=int(*)(RE::PlayerCharacter*);
void initializeFlightPose(FlightPoseGate,FlightPoseMode);
void configureFlightPoseTransitions(pose::TransitionTiming);
void installFlightPoseLoader();
void prepareFlightPose(RE::PlayerCharacter*,bool);
void beforeFlightPoseActivate(RE::hkbClipGenerator*,const RE::hkbContext&);
void afterFlightPoseActivate(RE::hkbClipGenerator*,const RE::hkbContext&);
void afterFlightPoseDeactivate(RE::hkbClipGenerator*,const RE::hkbContext&);
void updateFlightPose(RE::hkbClipGenerator*,const RE::hkbContext&);
bool flightPoseReady();
}
