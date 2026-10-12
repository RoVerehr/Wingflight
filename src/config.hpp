#pragma once
#include "flight.hpp"
#include "wing_motion.hpp"
#include "body_motion.hpp"
#include "leg_secondary_motion.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace wingflight {
struct WingProfile {
    std::string id, plugin;
    bool awuCompatibility{};
    bool nativeVampire{}; // Built-in race provider; no magic-effect art owner.
    bool continuousFlight{}; // Keep provider animation; held Jump controls ascent.
    float continuousRiseSpeed{4.0f};
    bool animationless{}; // Ordinary wingbeat physics/audio, no attached model.
    std::vector<std::uint32_t> effectIDs;
    std::string takeoff{"WingflightFlap"}, flap{"WingflightFlap"}, glide{"WingflightFlap"};
    std::string hover{"WingflightFlap"}, land{"WingflightRelease"};
    std::string fall{"WingflightFall"};
    std::string flapSequence{"mAnimation4_Loop"};
    std::string glideSequence{"mAnimation4s"};
    std::string spreadSequence{"mAnimation1_Loop"};
    std::uint32_t flapSoundID{0xC6B};
    std::uint32_t fastSoundID{};
    std::uint32_t spreadSoundID{};
    float flapSoundPhase{0.1894737f};
    float fastSoundPhase{0.20f};
    float spreadSoundPhase1{0.25f},spreadSoundPhase2{0.75f},spreadSoundVolume{1.0f};
    WingTiming timing;
};
struct Configuration {
    Tuning tuning;
    HoverTiming hoverTiming;
    BodyTiming bodyTiming;
    float powerStrokeWingPitch{45};
    float spreadAmplitudeMultiplier{1};
    bool enabled{true}, logSamples{true}, poseWhenArmed{true},faceFlight{true};
    bool awuCompatibility{true};
    bool glideEnabled{true};
    bool lowerBodyEnabled{true},lowerBodyLogClips{true};
    bool muteFlightFootsteps{true};
    bool muteFlightSprintBreath{true};
    bool armedFlightIdle{true};
    bool disableSprintWhenArmed{false};
    bool armedSprintLean{true};
    bool armedSprintAutoAlign{true};
    bool armedBanking{true};
    bool highSpeedPitch{true};
    float highSpeedPitchBlendSeconds{0.25f};
    float bankDegrees{20},bankBlendSeconds{0.35f};
    float armedSprintLeanDegrees{60},armedSprintLeanIn{0.75f},armedSprintLeanOut{0.75f};
    float lowerBodyBlendIn{0.30f},lowerBodyBlendOut{0.25f};
    legs::LegRelaxation legRelaxation;
    std::uint32_t modeKey{37}, landKey{46}, descendKey{29};
    std::uint32_t padMode{0}, padLand{0}, padDescend{0};
    float deadzone{0.2f},glideHoldSeconds{0.25f},jumpHeightMultiplier{2.0f};
    std::vector<WingProfile> profiles;
};
Configuration loadConfiguration();
}
