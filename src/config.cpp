#include "config.hpp"
#include "live_settings.hpp"
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <sstream>
#include <stdexcept>

namespace wingflight {
namespace {
std::string read(const std::filesystem::path& file, const char* section, const char* key, const char* fallback) {
    char buffer[8192]{};
    const auto count = GetPrivateProfileStringA(section, key, fallback, buffer, sizeof(buffer), file.string().c_str());
    if (count >= sizeof(buffer) - 2) { throw std::runtime_error(fmt::format("INI value exceeds 8190 bytes: file={} section={} key={}",file.string(),section,key)); }
    return buffer;
}
float number(const std::filesystem::path& file, const char* section, const char* key, float fallback, float low, float high) {
    const auto value = read(file, section, key, std::to_string(fallback).c_str());
    std::size_t used = 0;
    float parsed{};
    try {parsed=std::stof(value, &used);}
    catch(const std::exception& error){throw std::runtime_error(fmt::format("Cannot parse INI number: file={} section={} key={} value='{}' reason={}",file.string(),section,key,value,error.what()));}
    if (used != value.size() || !std::isfinite(parsed) || parsed < low || parsed > high) {
        throw std::runtime_error(fmt::format("Invalid INI value: {}/{} file={} value='{}' range={}..{}",section,key,file.string(),value,low,high));
    }
    return parsed;
}
unsigned long localID(const std::filesystem::path& file,const char* key,const std::string& text,std::size_t& used){
    try{return std::stoul(text,&used,16);}
    catch(const std::exception& error){throw std::runtime_error(fmt::format("Cannot parse local FormID: file={} key={} value='{}' reason={}",file.string(),key,text,error.what()));}
}
}
Configuration loadConfiguration() {
    Configuration cfg;
    const auto root = std::filesystem::absolute("Data/SKSE/Plugins");
    const auto file = root / "Wingflight.ini";
    auto integer = [&](const char* section, const char* key, unsigned fallback, unsigned maximum) {
        const float value = number(file, section, key, static_cast<float>(fallback), 0, static_cast<float>(maximum));
        if (std::floor(value) != value) { throw std::runtime_error(fmt::format("Non-integer setting: {}/{} value={}",section,key,value)); }
        return static_cast<unsigned>(value);
    };
    cfg.enabled = integer("General", "Enabled", 1, 1) != 0;
    cfg.awuCompatibility=integer("Compatibility","AWU",1,1)!=0;
    cfg.glideEnabled=integer("Glide","Enabled",1,1)!=0;
    cfg.logSamples = integer("General", "DiagnosticSamples", 1, 1) != 0;
    cfg.lowerBodyEnabled=integer("LowerBody","Enabled",1,1)!=0;
    cfg.lowerBodyLogClips=integer("LowerBody","LogAnimations",1,1)!=0;
    cfg.lowerBodyBlendIn=number(file,"LowerBody","BlendInSeconds",0.30f,0.05f,2);
    cfg.lowerBodyBlendOut=number(file,"LowerBody","BlendOutSeconds",0.25f,0.05f,2);
    auto& relaxation=cfg.legRelaxation;
    relaxation.enabled=integer("LowerBody","RelaxedArmedLegs",1,1)!=0;
    relaxation.thighAnimationWeight=number(file,"LowerBody","ThighAnimationWeight",0.35f,0,1);
    relaxation.secondaryMotion=integer("LowerBody","SecondaryMotion",1,1)!=0;
    relaxation.maxSwingDegrees=number(file,"LowerBody","SwingMaxDegrees",10,0,30);
    relaxation.stiffness=number(file,"LowerBody","SwingStiffness",18,1,100);
    relaxation.damping=number(file,"LowerBody","SwingDamping",6,0,40);
    relaxation.inertia=number(file,"LowerBody","SwingInertia",1,0,3);
    cfg.faceFlight=integer("Compatibility","TDMFlightFacing",1,1)!=0;
    cfg.muteFlightFootsteps=integer("Compatibility","MuteFlightFootsteps",1,1)!=0;
    cfg.muteFlightSprintBreath=integer("Compatibility","MuteFlightSprintBreath",1,1)!=0;
    cfg.armedFlightIdle=integer("Compatibility","ArmedFlightIdle",1,1)!=0;
    cfg.disableSprintWhenArmed=integer("Flight","DisableSprintWhenArmed",0,1)!=0;
    cfg.poseWhenArmed = integer("Compatibility", "PoseWhenWeaponsDrawn", 1, 1) != 0;
    cfg.modeKey = integer("Keyboard", "CycleMode", 37, 255);
    cfg.landKey = integer("Keyboard", "Land", 46, 255);
    cfg.descendKey = integer("Keyboard", "Descend", 29, 255);
    cfg.padMode = integer("Gamepad", "CycleMode", 0, 65535);
    cfg.padLand = integer("Gamepad", "Land", 0, 65535);
    cfg.padDescend = integer("Gamepad", "Descend", 0, 65535);
    if(const auto error=validateGamepadBindings({cfg.padMode,cfg.padLand,cfg.padDescend});!error.empty())
        throw std::runtime_error(error);
    for (const auto keys : {std::vector<unsigned>{cfg.modeKey, cfg.landKey, cfg.descendKey},
                            std::vector<unsigned>{cfg.padMode, cfg.padLand, cfg.padDescend}}) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            for (std::size_t j = i + 1; j < keys.size(); ++j) {
                if (keys[i] != 0 && keys[i] == keys[j]) { throw std::runtime_error(fmt::format("Duplicate flight key binding: code={} bindingIndices={}/{}",keys[i],i,j)); }
            }
        }
    }
    auto& body=cfg.bodyTiming;
    body.enabled=integer("Body","ForwardLean",1,1)!=0;
    cfg.armedSprintLean=integer("Body","ArmedSprintLean",1,1)!=0;
    cfg.armedSprintAutoAlign=integer("Body","ArmedSprintAutoAlign",1,1)!=0;
    cfg.armedSprintLeanDegrees=number(file,"Body","ArmedSprintLeanDegrees",60,0,90);
    cfg.armedSprintLeanIn=number(file,"Body","ArmedSprintLeanInSeconds",0.75f,0.05f,3);
    cfg.armedSprintLeanOut=number(file,"Body","ArmedSprintLeanOutSeconds",0.75f,0.05f,3);
    body.enterSpeedRatio=number(file,"Body","LeanEnterSpeedRatio",body.enterSpeedRatio,0.05f,1.0f);
    body.exitSpeedRatio=number(file,"Body","LeanExitSpeedRatio",body.exitSpeedRatio,0,body.enterSpeedRatio);
    body.enterDelay=number(file,"Body","LeanEnterDelay",body.enterDelay,0,2);
    body.exitDelay=number(file,"Body","LeanExitDelay",body.exitDelay,0,2);
    body.glideSpeed=number(file,"Wings","SpreadPlaybackMultiplier",body.glideSpeed,0.25f,4);
    cfg.spreadAmplitudeMultiplier=number(file,"Wings","SpreadAmplitudeMultiplier",1,0,2);
    body.forwardGlideSpeed=number(file,"Wings","FastFlapPlaybackMultiplier",body.forwardGlideSpeed,0.25f,4);
    cfg.powerStrokeWingPitch=number(file,"Wings","HighSpeedPowerStrokeWingPitch",45,0,90);
    body.banking=integer("Body","Banking",1,1)!=0;
    cfg.armedBanking=integer("Body","ArmedBanking",1,1)!=0 && body.banking;
    cfg.bankDegrees=number(file,"Body","BankDegrees",20,0,45);
    cfg.bankBlendSeconds=number(file,"Body","BankBlendSeconds",0.35f,0.05f,3);
    cfg.highSpeedPitch=integer("Body","HighSpeedPitch",1,1)!=0;
    cfg.highSpeedPitchBlendSeconds=number(file,"Body","HighSpeedPitchBlendSeconds",0.25f,0.05f,3);
    cfg.glideHoldSeconds=number(file,"Glide","HoldSeconds",0.25f,0.1f,1);

    cfg.deadzone = number(file, "Gamepad", "Deadzone", 0.2f, 0.05f, 0.8f);
    // Startup reads and live menu edits share exactly the same ranges/defaults.
    for(const auto& setting:liveFloatSettings)
        setting.value(cfg)=number(file,setting.section,setting.key,setting.value(cfg),setting.minimum,setting.maximum);
    auto& t = cfg.tuning;
    t.flapOnHighSpeedEntry=integer("Wings","FlapOnHighSpeedEntry",1,1)!=0;


    const auto directory = root / "Wingflight/Wings";
    if (!std::filesystem::is_directory(directory)) { return cfg; }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".ini") { paths.push_back(entry.path()); }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        WingProfile profile;
        profile.id = path.stem().string();
        profile.plugin = read(path, "Wings", "Plugin", "");
        const auto compatibility=read(path,"Wings","Compatibility",
            profile.plugin=="Animated Wings Ultimate.esp"?"AWU":"None");
        if(compatibility!="AWU" && compatibility!="None")throw std::runtime_error(fmt::format("Unknown compatibility module: file={} value='{}' expected=AWU/None",path.string(),compatibility));
        profile.awuCompatibility=compatibility=="AWU";
        const auto style=read(path,"Wings","FlightStyle","Wingbeats");
        if(style!="Wingbeats"&&style!="Continuous")throw std::runtime_error(fmt::format("Unknown wing flight style: file={} value='{}' expected=Wingbeats/Continuous",path.string(),style));
        profile.continuousFlight=style=="Continuous";
        profile.continuousRiseSpeed=number(path,"Wings","RiseSpeed",4,0.1f,100);
        const auto driver=read(path,"Wings","AnimationDriver","Runtime");
        if(driver!="Runtime"&&driver!="None")throw std::runtime_error(fmt::format("Unknown wing animation driver: file={} value='{}' expected=Runtime/None",path.string(),driver));
        profile.animationless=driver=="None";
        if(profile.animationless&&profile.continuousFlight)throw std::runtime_error(fmt::format("Continuous wings retain their provider loop; AnimationDriver=None is for ordinary invisible wingbeats: {}",path.string()));
        std::istringstream ids(read(path, "Wings", "Effects", ""));
        std::string item;
        while (std::getline(ids, item, ',')) {
            std::size_t used = 0;
            auto id = localID(path,"Effects",item,used);
            if (used != item.size() || id == 0 || id > 0xFFFFFF) { throw std::runtime_error(fmt::format("Invalid local effect ID: file={} key=Effects value='{}' expected=000001..FFFFFF",path.string(),item)); }
            profile.effectIDs.push_back(static_cast<std::uint32_t>(id));
        }
        if (profile.plugin.empty() || profile.effectIDs.empty()) { throw std::runtime_error(fmt::format("Empty wing adapter: file={} plugin='{}' effectCount={}",path.string(),profile.plugin,profile.effectIDs.size())); }
        profile.flapSequence = read(path, "Timing", "FlapSequence", profile.flapSequence.c_str());
        profile.glideSequence = read(path, "Timing", "FastSequence", profile.glideSequence.c_str());
        profile.spreadSequence = read(path, "Timing", "SpreadSequence", profile.spreadSequence.c_str());
        auto& timing=profile.timing;
        timing.expandSeconds=number(path,"Timing","ExpandSeconds",0.32f,0.05f,1.0f);
        timing.upstrokeSeconds=number(path,"Timing","UpstrokeSeconds",0.40f,0.10f,2.0f);
        timing.fastUpstrokeSeconds=number(path,"Timing","FastUpstrokeSeconds",0.20f,0.10f,timing.upstrokeSeconds);
        timing.downstrokeSeconds=number(path,"Timing","DownstrokeSeconds",0.34f,0.08f,2.0f);
        timing.fastDownstrokeSeconds=number(path,"Timing","FastDownstrokeSeconds",timing.downstrokeSeconds,0.08f,2.0f);
        timing.downstrokeEnd=number(path,"Timing","DownstrokeEnd",0.32f,0.1f,0.7f);
        {
            const auto value=read(path,"Sound","PowerStroke",profile.awuCompatibility?"000C6B":"000000");
            std::size_t used=0;
            profile.flapSoundID=localID(path,"PowerStroke",value,used);
            if(used!=value.size() || profile.flapSoundID>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid power-stroke sound ID: file={} value='{}' expected=000000..FFFFFF",path.string(),value));
            profile.flapSoundPhase=number(path,"Sound","MarkerPhase",0.1894737f,0,timing.downstrokeEnd-0.001f);
            const auto fastValue=read(path,"Sound","FastFlap","000000");
            used=0;profile.fastSoundID=localID(path,"FastFlap",fastValue,used);
            if(used!=fastValue.size()||profile.fastSoundID>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid fast-flap sound ID: file={} value='{}' expected=000000..FFFFFF",path.string(),fastValue));
            profile.fastSoundPhase=number(path,"Sound","FastMarkerPhase",0.20f,0,0.999f);
            const auto spreadValue=read(path,"Sound","SpreadFlutter","000000");
            used=0;profile.spreadSoundID=localID(path,"SpreadFlutter",spreadValue,used);
            if(used!=spreadValue.size()||profile.spreadSoundID>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid flutter sound ID: file={} value='{}' expected=000000..FFFFFF",path.string(),spreadValue));
            profile.spreadSoundPhase1=number(path,"Sound","SpreadMarkerPhase1",0.25f,0,0.998f);
            profile.spreadSoundPhase2=number(path,"Sound","SpreadMarkerPhase2",0.75f,profile.spreadSoundPhase1+0.001f,0.999f);
            profile.spreadSoundVolume=number(path,"Sound","SpreadVolume",1.0f,0,1);
        }
        timing.recoveryRate=number(path,"Timing","RecoveryRate",0.9f,0.2f,2.0f);
        timing.normalFlapSeconds=number(path,"Timing","NormalFlapSeconds",1.0f,0.2f,5.0f);

        timing.toFlapSeconds=number(path,"Timing","ToFlapSeconds",0.32f,0.10f,2.0f);
        timing.toGlideSeconds=number(path,"Timing","ToRestSeconds",0.40f,0.10f,2.0f);
        timing.glideEntryPhase=number(path,"Timing","FastEntryPhase",0.233333f,0,0.999f);
        cfg.profiles.push_back(std::move(profile));
    }
    if (cfg.profiles.empty())SKSE::log::info("No wing adapters configured; flight is available after installing a compatible wing adapter");
    return cfg;
}
}
