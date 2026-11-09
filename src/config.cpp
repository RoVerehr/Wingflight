#include "wing_audio_files.hpp"
#include "config.hpp"
#include "live_settings.hpp"
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <array>
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
float readNumber(const std::filesystem::path& file, const char* section, const char* key, float fallback, float low, float high) {
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
Configuration loadConfiguration(const std::filesystem::path& source,ConfigurationReadOptions options) {
    Configuration cfg;
    const auto root = std::filesystem::absolute("Data/SKSE/Plugins");
    const auto file = source.empty()?root / "Wingflight.ini":std::filesystem::absolute(source);
    if(options.recoveredSettings)*options.recoveredSettings=false;
    const auto invalid=[&](std::string_view error){
        if(!options.recoverInvalidSettings)throw std::runtime_error(std::string(error));
        if(options.recoveredSettings)*options.recoveredSettings=true;
        SKSE::log::error("Invalid player setting ignored: file={} reason={}; using its default or disabling the conflicting binding; source unchanged",file.string(),error);
    };
    const auto mainText=[&](const char* section,const char* key,const char* fallback){
        if(options.useDefaults)return std::string(fallback);
        try{return read(file,section,key,fallback);}
        catch(const std::exception& error){invalid(error.what());return std::string(fallback);}
    };
    const auto number=[&](const char* section,const char* key,float fallback,float low,float high){
        if(options.useDefaults)return fallback;
        try{return readNumber(file,section,key,fallback,low,high);}
        catch(const std::exception& error){invalid(error.what());return fallback;}
    };
    auto integer = [&](const char* section, const char* key, unsigned fallback, unsigned maximum,unsigned minimum=0) {
        const float value = number(section,key,static_cast<float>(fallback),static_cast<float>(minimum),static_cast<float>(maximum));
        if(std::floor(value)!=value){invalid(fmt::format("Non-integer setting: {}/{} value={} default={}",section,key,value,fallback));return fallback;}
        return static_cast<unsigned>(value);
    };
    cfg.controlJumpWingAnimations=integer("Jump","ControlWingAnimations",cfg.controlJumpWingAnimations,1)!=0;
    cfg.foldWingsWhileSneaking=integer("Jump","FoldWingsWhileSneaking",cfg.foldWingsWhileSneaking,1)!=0;
    cfg.enabled = integer("General", "Enabled", cfg.enabled, 1) != 0;
    cfg.actors.enabled=integer("NPCFlight","Enabled",cfg.actors.enabled,1)!=0;
    cfg.actors.airCombat=integer("NPCFlight","AirCombat",cfg.actors.airCombat,1)!=0;
    cfg.actors.autoWings=integer("NPCFlight","AutoWings",cfg.actors.autoWings,1)!=0;
    try{
        const auto autoWingID=mainText("NPCFlight","AutoWingsEffect","00089A");
        std::size_t autoWingUsed{};
        const auto autoWingForm=localID(file,"AutoWingsEffect",autoWingID,autoWingUsed);
        if(autoWingUsed!=autoWingID.size()||autoWingForm==0||autoWingForm>0xFFFFFF)
            throw std::runtime_error("NPCFlight/AutoWingsEffect must be a local hexadecimal AWU effect ID (000001..FFFFFF)");
        cfg.actors.autoWingsEffect=static_cast<std::uint32_t>(autoWingForm);
    }catch(const std::exception& error){invalid(error.what());}
    cfg.actors.maxActors=integer("NPCFlight","MaxActors",cfg.actors.maxActors,64,1);
    cfg.actors.followDistance=number("NPCFlight","FollowDistance",cfg.actors.followDistance,70,2000);
    cfg.collision.enabled=integer("Collision","Enabled",cfg.collision.enabled,1)!=0;
    cfg.collision.damageActors=integer("Collision","DamageActors",cfg.collision.damageActors,1)!=0;
    cfg.awuCompatibility=integer("Compatibility","AWU",cfg.awuCompatibility,1)!=0;
    cfg.glideEnabled=integer("Glide","Enabled",cfg.glideEnabled,1)!=0;
    cfg.logSamples = integer("General", "DetailedLogging", cfg.logSamples, 1) != 0;
    cfg.lowerBodyEnabled=integer("LowerBody","Enabled",cfg.lowerBodyEnabled,1)!=0;
    cfg.lowerBodyBlendIn=number("LowerBody","BlendInSeconds",cfg.lowerBodyBlendIn,0.05f,2);
    cfg.lowerBodyBlendOut=number("LowerBody","BlendOutSeconds",cfg.lowerBodyBlendOut,0.05f,2);
    auto& relaxation=cfg.legRelaxation;
    relaxation.enabled=integer("LowerBody","RelaxedArmedLegs",relaxation.enabled,1)!=0;
    relaxation.thighAnimationWeight=number("LowerBody","ThighAnimationWeight",relaxation.thighAnimationWeight,0,1);
    relaxation.secondaryMotion=integer("LowerBody","SecondaryMotion",relaxation.secondaryMotion,1)!=0;
    relaxation.maxSwingDegrees=number("LowerBody","SwingMaxDegrees",relaxation.maxSwingDegrees,0,30);
    relaxation.stiffness=number("LowerBody","SwingStiffness",relaxation.stiffness,1,100);
    relaxation.damping=number("LowerBody","SwingDamping",relaxation.damping,0,40);
    relaxation.inertia=number("LowerBody","SwingInertia",relaxation.inertia,0,3);
    cfg.faceFlight=integer("Compatibility","TDMFlightFacing",cfg.faceFlight,1)!=0;
    cfg.muteFlightFootsteps=integer("Compatibility","MuteFlightFootsteps",cfg.muteFlightFootsteps,1)!=0;
    cfg.muteFlightSprintBreath=integer("Compatibility","MuteFlightSprintBreath",cfg.muteFlightSprintBreath,1)!=0;
    cfg.armedFlightIdle=integer("Compatibility","ArmedFlightIdle",cfg.armedFlightIdle,1)!=0;
    cfg.disableSprintWhenArmed=integer("Flight","DisableSprintWhenArmed",cfg.disableSprintWhenArmed,1)!=0;
    cfg.tuning.allowOverencumberedFlight=integer("Flight","AllowOverencumberedFlight",cfg.tuning.allowOverencumberedFlight,1)!=0;
    cfg.tuning.glideDeceleration=number("Flight","GlideDeceleration",cfg.tuning.glideDeceleration,0.1f,30);
    cfg.tuning.endFlightOnGround=integer("Flight","EndFlightOnGround",cfg.tuning.endFlightOnGround,1)!=0;
    cfg.tuning.groundWingIdle=integer("Flight","GroundWingIdle",cfg.tuning.groundWingIdle,1)!=0;
    cfg.poseWhenArmed = integer("Compatibility", "PoseWhenWeaponsDrawn", cfg.poseWhenArmed, 1) != 0;
    cfg.modeKey = integer("Keyboard", "CycleMode", cfg.modeKey, 255);
    cfg.landKey = integer("Keyboard", "Land", cfg.landKey, 255);
    cfg.descendKey = integer("Keyboard", "Descend", cfg.descendKey, 255);
    cfg.padMode = integer("Gamepad", "CycleMode", cfg.padMode, 65535);
    cfg.padLand = integer("Gamepad", "Land", cfg.padLand, 65535);
    cfg.padDescend = integer("Gamepad", "Descend", cfg.padDescend, 65535);
    for(const auto binding:{&cfg.padMode,&cfg.padLand,&cfg.padDescend})if(!validGamepadButton(*binding)){
        invalid(fmt::format("Unsupported gamepad binding: code={}; expected a single supported button or 0",*binding));*binding=0;
    }
    for (const auto keys : {std::array{&cfg.modeKey,&cfg.landKey,&cfg.descendKey},
                            std::array{&cfg.padMode,&cfg.padLand,&cfg.padDescend}}) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            for (std::size_t j = i + 1; j < keys.size(); ++j) {
                if(*keys[i]!=0&&*keys[i]==*keys[j]){
                    invalid(fmt::format("Duplicate flight key binding: code={} bindingIndices={}/{}",*keys[i],i,j));*keys[j]=0;
                }
            }
        }
    }
    auto& body=cfg.bodyTiming;
    body.enabled=mainText("Body","UnarmedFlightPoses","").empty()&&!mainText("Body","ForwardLean","").empty()
        ?integer("Body","ForwardLean",body.enabled,1)!=0:integer("Body","UnarmedFlightPoses",body.enabled,1)!=0;
    cfg.movementBlendSeconds=number("Body","MovementBlendSeconds",cfg.movementBlendSeconds,0.05f,3);
    cfg.sprintBlendInSeconds=number("Body","SprintBlendInSeconds",cfg.sprintBlendInSeconds,0.05f,3);
    cfg.sprintBlendOutSeconds=number("Body","SprintBlendOutSeconds",cfg.sprintBlendOutSeconds,0.05f,3);
    cfg.armedSprintLean=integer("Body","ArmedSprintLean",cfg.armedSprintLean,1)!=0;
    cfg.armedSprintAutoAlign=integer("Body","ArmedSprintAutoAlign",cfg.armedSprintAutoAlign,1)!=0;
    cfg.armedSprintLeanDegrees=number("Body","ArmedSprintLeanDegrees",cfg.armedSprintLeanDegrees,0,90);
    cfg.armedSprintLeanIn=number("Body","ArmedSprintLeanInSeconds",cfg.armedSprintLeanIn,0.05f,3);
    cfg.armedSprintLeanOut=number("Body","ArmedSprintLeanOutSeconds",cfg.armedSprintLeanOut,0.05f,3);
    body.glideSpeed=number("Wings","SpreadPlaybackMultiplier",body.glideSpeed,0.25f,4);
    cfg.spreadAmplitudeMultiplier=number("Wings","SpreadAmplitudeMultiplier",cfg.spreadAmplitudeMultiplier,0,2);
    body.forwardGlideSpeed=number("Wings","FastFlapPlaybackMultiplier",body.forwardGlideSpeed,0.25f,4);
    cfg.powerStrokeWingPitch=number("Wings","HighSpeedPowerStrokeWingPitch",cfg.powerStrokeWingPitch,0,90);
    cfg.highSpeedFlapHoldSeconds=number("Wings","HighSpeedFlapHoldSeconds",cfg.highSpeedFlapHoldSeconds,0,5);
    body.banking=integer("Body","Banking",body.banking,1)!=0;
    cfg.armedBanking=integer("Body","ArmedBanking",cfg.armedBanking,1)!=0 && body.banking;
    cfg.bankDegrees=number("Body","BankDegrees",cfg.bankDegrees,0,45);
    cfg.bankBlendSeconds=number("Body","BankBlendSeconds",cfg.bankBlendSeconds,0.05f,3);
    cfg.highSpeedPitch=integer("Body","HighSpeedPitch",cfg.highSpeedPitch,1)!=0;
    cfg.highSpeedPitchBlendSeconds=number("Body","HighSpeedPitchBlendSeconds",cfg.highSpeedPitchBlendSeconds,0.05f,3);
    cfg.glideHoldSeconds=number("Glide","HoldSeconds",cfg.glideHoldSeconds,0.1f,1);

    cfg.deadzone = number( "Gamepad", "Deadzone", cfg.deadzone, 0.05f, 0.8f);
    // The ground-only target is now shared by full-flight descent. Preserve a
    // tuned main-INI value; the canonical rewrite emits only the new key.
    if(mainText("Flight","DescentSpeed","").empty()&&!mainText("Flight","GroundDescentSpeed","").empty())
        cfg.tuning.descentSpeed=number("Flight","GroundDescentSpeed",cfg.tuning.descentSpeed,.5f,8.f);
    // Startup reads and live menu edits share exactly the same ranges/defaults.
    for(const auto& setting:liveFloatSettings)
        setting.value(cfg)=number(setting.section,setting.key,setting.value(cfg),setting.minimum,setting.maximum);
    auto& t = cfg.tuning;
    t.flapOnHighSpeedEntry=integer("Wings","FlapOnHighSpeedEntry",t.flapOnHighSpeedEntry,1)!=0;


    const auto directory = root / "Wingflight/Wings";
    std::error_code directoryError;
    if(!std::filesystem::is_directory(directory,directoryError)){
        if(directoryError&&directoryError!=std::errc::no_such_file_or_directory)SKSE::log::error("Wing adapter directory ignored: path={} reason={}",directory.string(),directoryError.message());
        return cfg;
    }
    std::vector<std::filesystem::path> paths;
    for(std::filesystem::directory_iterator it(directory,directoryError),end;it!=end&&!directoryError;it.increment(directoryError)){
        if(it->path().extension()==".ini")paths.push_back(it->path());
    }
    if(directoryError)SKSE::log::error("Wing adapter directory read incomplete: path={} reason={}; readable adapters retained",directory.string(),directoryError.message());
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
      try{
        // New registrations have their own parser and stable runtime storage.
        // Legacy providers keep their existing defaults and AWU handoff rules.
        if(!read(path,"Wings","Template","").empty())continue;
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
        profile.continuousRiseSpeed=readNumber(path,"Wings","RiseSpeed",4,0.1f,100);
        const auto driver=read(path,"Wings","AnimationDriver","Runtime");
        if(driver!="Runtime"&&driver!="None")throw std::runtime_error(fmt::format("Unknown wing animation driver: file={} value='{}' expected=Runtime/None",path.string(),driver));
        profile.animationless=driver=="None";
        std::istringstream arts(read(path,"Wings","ArtObjects",""));
        std::string art;
        while(std::getline(arts,art,',')){
            std::size_t used{};const auto id=localID(path,"ArtObjects",art,used);
            if(used!=art.size()||id==0||id>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid wing ArtObjects ID: file={} value='{}'",path.string(),art));
            profile.artObjectIDs.push_back(static_cast<std::uint32_t>(id));
        }
        profile.soundPlugin=read(path,"Sound","Plugin",profile.plugin.c_str());
        profile.flapSoundFiles=wingSoundFiles(read(path,"Sound","PowerStrokeFiles",""));
        profile.fastSoundFiles=wingSoundFiles(read(path,"Sound","FastFlapFiles",""));
        profile.spreadSoundFiles=wingSoundFiles(read(path,"Sound","SpreadFlutterFiles",""));
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
        timing.expandSeconds=readNumber(path,"Timing","ExpandSeconds",0.32f,0.05f,1.0f);
        timing.upstrokeSeconds=readNumber(path,"Timing","UpstrokeSeconds",0.40f,0.10f,2.0f);
        timing.fastUpstrokeSeconds=readNumber(path,"Timing","FastUpstrokeSeconds",0.20f,0.10f,timing.upstrokeSeconds);
        timing.downstrokeSeconds=readNumber(path,"Timing","DownstrokeSeconds",0.34f,0.08f,2.0f);
        timing.fastDownstrokeSeconds=readNumber(path,"Timing","FastDownstrokeSeconds",timing.downstrokeSeconds,0.08f,2.0f);
        timing.downstrokeEnd=readNumber(path,"Timing","DownstrokeEnd",0.32f,0.1f,0.7f);
        {
            const auto value=read(path,"Sound","PowerStroke",profile.awuCompatibility?"000C6B":"000000");
            std::size_t used=0;
            profile.flapSoundID=localID(path,"PowerStroke",value,used);
            if(used!=value.size() || profile.flapSoundID>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid power-stroke sound ID: file={} value='{}' expected=000000..FFFFFF",path.string(),value));
            profile.flapSoundPhase=readNumber(path,"Sound","MarkerPhase",0.1894737f,0,timing.downstrokeEnd-0.001f);
            const auto fastValue=read(path,"Sound","FastFlap","000000");
            used=0;profile.fastSoundID=localID(path,"FastFlap",fastValue,used);
            if(used!=fastValue.size()||profile.fastSoundID>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid fast-flap sound ID: file={} value='{}' expected=000000..FFFFFF",path.string(),fastValue));
            profile.fastSoundPhase=readNumber(path,"Sound","FastMarkerPhase",0.20f,0,0.999f);
            const auto spreadValue=read(path,"Sound","SpreadFlutter","000000");
            used=0;profile.spreadSoundID=localID(path,"SpreadFlutter",spreadValue,used);
            if(used!=spreadValue.size()||profile.spreadSoundID>0xFFFFFF)throw std::runtime_error(fmt::format("Invalid flutter sound ID: file={} value='{}' expected=000000..FFFFFF",path.string(),spreadValue));
            profile.spreadSoundPhase1=readNumber(path,"Sound","SpreadMarkerPhase1",0.25f,0,0.998f);
            profile.spreadSoundPhase2=readNumber(path,"Sound","SpreadMarkerPhase2",0.75f,profile.spreadSoundPhase1+0.001f,0.999f);
            profile.spreadSoundVolume=readNumber(path,"Sound","SpreadVolume",1.0f,0,1);
        }
        timing.recoveryRate=readNumber(path,"Timing","RecoveryRate",0.9f,0.2f,2.0f);
        timing.normalFlapSeconds=readNumber(path,"Timing","NormalFlapSeconds",1.0f,0.2f,5.0f);

        timing.toFlapSeconds=readNumber(path,"Timing","ToFlapSeconds",0.32f,0.10f,2.0f);
        timing.toGlideSeconds=readNumber(path,"Timing","ToRestSeconds",0.40f,0.10f,2.0f);
        timing.glideEntryPhase=readNumber(path,"Timing","FastEntryPhase",0.233333f,0,0.999f);
        cfg.profiles.push_back(std::move(profile));
      }catch(const std::exception& error){
        SKSE::log::error("Wing adapter ignored: file={} reason={}; other adapters retained",path.string(),error.what());
      }
    }
    if (cfg.profiles.empty())SKSE::log::info("No wing adapters configured; flight is available after installing a compatible wing adapter");
    return cfg;
}
}
