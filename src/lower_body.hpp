#pragma once
#include "leg_secondary_motion.hpp"
namespace wingflight {
struct Configuration;
void installLowerBody(const Configuration& config);
void applyLowerBodySettings(const Configuration& config);
void prepareLowerBody(RE::PlayerCharacter* player,bool fullFlight,float dt,bool highSpeed=false,float bankTurn=0,float flightPitch=0,legs::LegMotionInput motion={});
float armedSprintLeanAngle();
float flightBankAngle();
float flightPitchAngle();
void resetLowerBody();
void observeLowerBodyClip(RE::hkbClipGenerator* clip,const RE::hkbContext& context);
void forgetLowerBodyClip(RE::hkbClipGenerator* clip);
}
