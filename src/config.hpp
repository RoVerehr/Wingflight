#pragma once
#include "wing_channel_policy.hpp"
#include "wing_appearance_policy.hpp"
#include "flight.hpp"
#include "wing_motion.hpp"
#include "body_motion.hpp"
#include "leg_secondary_motion.hpp"
#include "collision_policy.hpp"
#include "actor_flight_policy.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace wingflight {
struct WingProfile {
    std::string id, plugin;
    channel::Template wingTemplate{channel::Template::legacy};
    channel::PoseSettings wingPose;
    std::string registrationFile,rootNode,soundPlugin;
    std::vector<std::string> leftRoots,rightRoots;
    std::vector<std::uint32_t> armorIDs;
    bool awuCompatibility{};
    bool nativeVampire{}; // Built-in race provider; no magic-effect art owner.
    bool continuousFlight{}; // Steady hover; held Jump controls ascent, independently of wing animation.
    float continuousRiseSpeed{4.0f};
    bool animationless{}; // No attached wing model; supports wingbeat or continuous flight.
    std::vector<std::uint32_t> effectIDs;
    std::vector<std::uint32_t> artObjectIDs; // Optional explicit selectors for script-generated visuals.
    std::string takeoff{"WingflightFlap"}, flap{"WingflightFlap"}, glide{"WingflightFlap"};
    std::string hover{"WingflightFlap"}, land{"WingflightRelease"};
    std::string fall{"WingflightFall"};
    std::string flapSequence{"mAnimation4_Loop"};
    std::string glideSequence{"mAnimation4s"};
    std::string spreadSequence{"mAnimation1_Loop"};
    std::uint32_t flapSoundID{0xC6B};
    std::uint32_t fastSoundID{};
    std::uint32_t spreadSoundID{};
    std::vector<std::string> flapSoundFiles,fastSoundFiles,spreadSoundFiles;
    float flapSoundPhase{0.1894737f};
    float fastSoundPhase{0.20f};
    float spreadSoundPhase1{0.25f},spreadSoundPhase2{0.75f},spreadSoundVolume{1.0f};
    WingTiming timing;
};
struct Configuration {
    appearance::Settings wingAppearance;
    aerial::Settings actors;
    impact::Settings collision;
    Tuning tuning;
    HoverTiming hoverTiming;
    BodyTiming bodyTiming;
    float powerStrokeWingPitch{30};
    float highSpeedFlapHoldSeconds{0.5f};
    bool controlJumpWingAnimations{true};
    bool foldWingsWhileSneaking{false};
    float jumpWingOpenPercent{15};
    float jumpWingOverOpenPercent{30},jumpWingOpenSeconds{.4f},jumpWingSettleSeconds{1.f};
    float jumpWingFlutterPeriod{1.f},jumpWingFlutterAmplitude{1};
    float spreadAmplitudeMultiplier{.5f};
    bool enabled{true}, logSamples{false}, poseWhenArmed{true},faceFlight{true};
    bool awuCompatibility{true};
    bool glideEnabled{true};
    bool lowerBodyEnabled{true};
    bool muteFlightFootsteps{true};
    bool muteFlightSprintBreath{true};
    bool armedFlightIdle{true};
    bool disableSprintWhenArmed{false};
    bool armedSprintLean{true};
    bool armedSprintAutoAlign{true};
    bool armedBanking{true};
    bool highSpeedPitch{true};
    float highSpeedPitchBlendSeconds{0.25f};
    float movementBlendSeconds{0.75f},sprintBlendInSeconds{0.75f},sprintBlendOutSeconds{0.75f};
    float bankDegrees{20},bankBlendSeconds{0.35f};
    float armedSprintLeanDegrees{60},armedSprintLeanIn{0.5f},armedSprintLeanOut{0.5f};
    float lowerBodyBlendIn{0.30f},lowerBodyBlendOut{0.25f};
    legs::LegRelaxation legRelaxation;
    std::uint32_t modeKey{37}, landKey{46}, descendKey{29};
    std::uint32_t padMode{0}, padLand{0}, padDescend{0};
    float deadzone{0.2f},glideHoldSeconds{0.25f},jumpHeightMultiplier{1.0f};
    std::vector<WingProfile> profiles;
};
// Startup may recover individual settings without changing the source file.
// Menu transactions keep strict main-setting validation; adapters are isolated.
struct ConfigurationReadOptions {
    bool useDefaults{};
    bool recoverInvalidSettings{};
    bool* recoveredSettings{};
};
Configuration loadConfiguration(const std::filesystem::path& source = {},ConfigurationReadOptions options = {});
}
