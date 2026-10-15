#pragma once
#include "config.hpp"
#include "gamepad_bindings.hpp"
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wingflight {
struct LiveFloatSetting {
    const char* section;
    const char* key;
    const char* help;
    float minimum, maximum;
    float& (*value)(Configuration&);
};
// This is the complete menu allowlist. All other settings remain startup-only.
inline constexpr auto liveFloatSettings = std::to_array<LiveFloatSetting>({
    {"NPCFlight","FollowDistance","Distance behind the player's current position, in Skyrim units (70 units is about one metre). Saved values apply to airborne followers immediately.",70,2000,[](Configuration& c)->float&{return c.actors.followDistance;}},
    {"Jump","OverOpenPercent","Initial jump wing opening, independently set from 0 to 100. 100 opens fully, then WingSettleSeconds transitions to WingOpenPercent. Set both percentages equal for no return motion.",0,100,[](Configuration& c)->float&{return c.jumpWingOverOpenPercent;}},
    {"Jump","WingOpenSeconds","Seconds to ease into the jump opening peak; also used to fold on landing.",.05f,3,[](Configuration& c)->float&{return c.jumpWingOpenSeconds;}},
    {"Jump","WingSettleSeconds","Seconds to transition from OverOpenPercent to WingOpenPercent.",.05f,3,[](Configuration& c)->float&{return c.jumpWingSettleSeconds;}},
    {"Jump","WingFlutterPeriod","Seconds per jump wing flutter cycle. Smaller is faster; phase continues when adjusted. Uses the same authored spread loop as glide.",.2f,5,[](Configuration& c)->float&{return c.jumpWingFlutterPeriod;}},
    {"Jump","WingFlutterAmplitude","Jump-only flutter amplitude: 0 = still, 1 = source motion, 2 = double. Independent of SpreadAmplitudeMultiplier. The wing opening pose blend still reduces visible motion at partial opening.",0,2,[](Configuration& c)->float&{return c.jumpWingFlutterAmplitude;}},
    {"Jump","WingOpenPercent","Settled AWU dragon/feather wing opening during ordinary jumps: 0 = folded, 30 = subtle, 100 = fully spread. OverOpenPercent independently sets the initial opening. Does not change flight/glide, lift or jump height.",0,100,[](Configuration& c)->float&{return c.jumpWingOpenPercent;}},
    {"Collision","Multiplier","Collision damage scale for the player and contacted actors. 0 disables damage.",0,10,[](Configuration& c)->float&{return c.collision.multiplier;}},
    {"Collision","MinImpactSpeed","Safe closing speed into a surface, in Havok units. Tangential speed does not count. Base damage = 5 * (closing speed - this value)^2 * Multiplier, then native damage rules apply.",0,100,[](Configuration& c)->float&{return c.collision.minSpeed;}},
    {"Flight","Speed","Ordinary flight speed in Havok velocity units.",0.5f,20,[](Configuration& c)->float&{return c.tuning.speed;}},
    {"Flight","SprintSpeedMultiplier","Multiplier while the actor is actually sprinting; wingbeats add a separate bonus.",1,4,[](Configuration& c)->float&{return c.tuning.sprintSpeedMultiplier;}},
    {"Flight","TurnResponse","Horizontal steering response. Larger values turn faster.",0.1f,60,[](Configuration& c)->float&{return c.tuning.turnResponse;}},
    {"Flight","Acceleration","Response when accelerating toward the requested speed.",0.1f,30,[](Configuration& c)->float&{return c.tuning.acceleration;}},
    {"Flight","Drag","Response when slowing down after movement input ends.",0.1f,30,[](Configuration& c)->float&{return c.tuning.drag;}},
    {"Flight","FlapImpulse","Vertical velocity budget per large wingbeat, delivered during its downstroke. Not a height distance.",0.1f,10,[](Configuration& c)->float&{return c.tuning.lift;}},
    {"Flight","MaxRise","Upper bound for wingbeat-added upward velocity, not height. Inherited faster native upward momentum is not abruptly braked in free flight.",0.5f,15,[](Configuration& c)->float&{return c.tuning.maxRise;}},
    {"Flight","Gravity","Shared simulated downward acceleration (Havok units/s^2) in manual and free flight. Native player gravity is temporarily disabled during full flight.",0.1f,20,[](Configuration& c)->float&{return c.tuning.gravity;}},
    {"Flight","GlideFallSpeed","Maximum slow-descent velocity in manual glide, free-flight held Descend, and outside-flight held-Jump glide.",0.1f,8,[](Configuration& c)->float&{return c.tuning.glideFall;}},
    {"Flight","FlapCooldown","Minimum seconds between accepted manual wingbeats.",0.1f,2,[](Configuration& c)->float&{return c.tuning.flapCooldown;}},
    {"Flight","FlapSpeedMultiplier","Each accepted sprint wingbeat adds this value minus 1 to the extra speed multiplier.",1,3,[](Configuration& c)->float&{return c.tuning.flapSpeedMultiplier;}},
    {"Flight","MaxFlapSpeedMultiplier","Upper limit of the separate wingbeat speed bonus.",1,4,[](Configuration& c)->float&{return c.tuning.maxFlapSpeedMultiplier;}},
    {"Flight","FlapSpeedHoldSeconds","Seconds to hold the wingbeat speed bonus before it fades.",0,10,[](Configuration& c)->float&{return c.tuning.flapSpeedHoldSeconds;}},
    {"Flight","FlapSpeedFadeSeconds","Seconds used by the wingbeat speed bonus fade.",0.1f,10,[](Configuration& c)->float&{return c.tuning.flapSpeedFadeSeconds;}},
    {"FreeFlight","UpstrokeSeconds","Automatic hovering wingbeat: wing-raising duration in seconds.",0.1f,3,[](Configuration& c)->float&{return c.hoverTiming.upstrokeSeconds;}},
    {"FreeFlight","DownstrokeSeconds","Automatic hovering wingbeat: downward stroke duration in seconds.",0.08f,2,[](Configuration& c)->float&{return c.hoverTiming.downstrokeSeconds;}},
    {"FreeFlight","RecoveryRate","Maximum recovery playback speed. Automatically slows to bridge the predicted wait for the next hover wingbeat; does not set fall speed.",0.2f,2,[](Configuration& c)->float&{return c.hoverTiming.recoveryRate;}},
    {"FreeFlight","AutoFlapImpulse","Automatic wingbeat velocity budget. Uses Flight/Gravity and MaxRise; native gravity is disabled during full flight.",0.1f,100,[](Configuration& c)->float&{return c.tuning.autoFlapImpulse;}},
    {"FreeFlight","AutoFlapCost","Stamina once when a visible automatic downstroke starts. 0 makes automatic beats free; manual Space uses Stamina/FlapCost.",0,100,[](Configuration& c)->float&{return c.tuning.autoFlapCost;}},
    {"Stamina","FlapCost","Stamina once when a visible manual downstroke starts, including takeoff and automatic high-speed entry. 0 disables this cost.",0,100,[](Configuration& c)->float&{return c.tuning.flapCost;}},
    {"Stamina","SustainPerSecond","Stamina consumed per second in free-flight mode. Manual flight and outside-flight held glide do not pay this cost. 0 disables it.",0,100,[](Configuration& c)->float&{return c.tuning.sustainCost;}},
    {"Jump","WingedHeightMultiplier","Multiplier of native ground-jump HEIGHT with supported wings. 1 disables the extra boost; AWU's boost may stack.",1,4,[](Configuration& c)->float&{return c.jumpHeightMultiplier;}},
    {"LowerBody","BlendInSeconds","Seconds to blend flight legs in, including armed relaxation when drawing a weapon.",0.05f,2,[](Configuration& c)->float&{return c.lowerBodyBlendIn;}},
    {"LowerBody","BlendOutSeconds","Seconds to release the flight-leg overlay or armed relaxation when sheathing.",0.05f,2,[](Configuration& c)->float&{return c.lowerBodyBlendOut;}},
    {"LowerBody","ThighAnimationWeight","Authored thigh animation retained during armed flight: 0 = current flight animation, 1 = original weapon/attack thighs.",0,1,[](Configuration& c)->float&{return c.legRelaxation.thighAnimationWeight;}},
    {"LowerBody","SwingMaxDegrees","Maximum additional calf rotation about the knee hinge, relative to its flight pose. Feet receive a small ankle correction.",0,30,[](Configuration& c)->float&{return c.legRelaxation.maxSwingDegrees;}},
    {"LowerBody","SwingStiffness","Restoring spring strength. Larger values return to the target more firmly/quickly.",1,100,[](Configuration& c)->float&{return c.legRelaxation.stiffness;}},
    {"LowerBody","SwingDamping","Angular damping. Larger values reduce wobble; 0 allows continued oscillation.",0,40,[](Configuration& c)->float&{return c.legRelaxation.damping;}},
    {"LowerBody","SwingInertia","Acceleration and parent-rotation influence. Larger values make swing more noticeable; 0 removes driven swing.",0,3,[](Configuration& c)->float&{return c.legRelaxation.inertia;}}
});

inline std::string validateLiveSettings(Configuration candidate) {
    for (const auto& setting : liveFloatSettings) {
        const auto value=setting.value(candidate);
        if (!std::isfinite(value) || value<setting.minimum || value>setting.maximum)
            return std::string(setting.section)+"/"+setting.key+" is out of range: value="+std::to_string(value)
                +" expected="+std::to_string(setting.minimum)+".."+std::to_string(setting.maximum)+".";
    }
    if (candidate.modeKey>255 || candidate.landKey>255 || candidate.descendKey>255) return "Keyboard scan codes must be in 0..255.";
    const auto keys=std::array{candidate.modeKey,candidate.landKey,candidate.descendKey};
    for(std::size_t i=0;i<keys.size();++i)
        for(std::size_t j=i+1;j<keys.size();++j)
            if(keys[i]!=0 && keys[i]==keys[j]) return "CycleMode, Land and Descend must use different keys. Duplicate scan code="+std::to_string(keys[i])+".";
    if(const auto error=validateGamepadBindings({candidate.padMode,candidate.padLand,candidate.padDescend});!error.empty())return error;
    return {};
}

// Use the same decimal conversion as INI output so live and reloaded values
// agree exactly, including halfway cases and floating-point representation.
inline void roundLiveSettings(Configuration& settings) {
    for(const auto& setting:liveFloatSettings) {
        auto& value=setting.value(settings);
        if(!std::isfinite(value))continue; // Validation still rejects invalid input.
        char text[64];
        const auto [end,writeError]=std::to_chars(text,text+sizeof(text),value,std::chars_format::fixed,2);
        float rounded{};
        if(writeError!=std::errc{})throw std::runtime_error("Cannot round a setting.");
        const auto [parsed,readError]=std::from_chars(text,end,rounded);
        if(readError!=std::errc{}||parsed!=end)throw std::runtime_error("Cannot read a rounded setting.");
        value=rounded;
    }
}

inline void applyLiveSettings(Configuration& current, Configuration candidate, float /*inverseWorldScale*/) {
    roundLiveSettings(candidate);
    for (const auto& setting : liveFloatSettings) setting.value(current)=setting.value(candidate);
    current.disableSprintWhenArmed=candidate.disableSprintWhenArmed;
    current.actors.enabled=candidate.actors.enabled;
    current.actors.airCombat=candidate.actors.airCombat;
    current.logSamples=candidate.logSamples;
    current.collision.enabled=candidate.collision.enabled;
    current.collision.damageActors=candidate.collision.damageActors;
    current.legRelaxation.enabled=candidate.legRelaxation.enabled;
    current.legRelaxation.secondaryMotion=candidate.legRelaxation.secondaryMotion;
    current.modeKey=candidate.modeKey;current.landKey=candidate.landKey;current.descendKey=candidate.descendKey;
    current.padMode=candidate.padMode;current.padLand=candidate.padLand;current.padDescend=candidate.padDescend;
}
}
