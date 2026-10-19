#include "wing_registration_file.hpp"
#include "wing_model_names.hpp"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace wingflight {
namespace {
std::string read(const std::filesystem::path& path,const char* section,const char* key,const char* fallback=""){
    char text[8192]{};const auto size=GetPrivateProfileStringA(section,key,fallback,text,sizeof(text),path.string().c_str());
    if(size>=sizeof(text)-2)throw std::runtime_error("Wing registration value exceeds 8 KiB");
    return text;
}
std::vector<std::string> names(std::string text){
    std::istringstream stream(text);std::string value;std::vector<std::string> out;
    while(std::getline(stream,value,',')){
        const auto first=value.find_first_not_of(" \t\r"),last=value.find_last_not_of(" \t\r");
        if(first==value.npos)continue;
        value=value.substr(first,last-first+1);
        if(!channel::storableWingName(value))throw std::runtime_error("Invalid wing bone name");
        out.push_back(value);
    }
    if(out.size()>128)throw std::runtime_error("Wing registration list exceeds 128 entries");
    return out;
}
std::vector<std::uint32_t> ids(const std::string& text){
    std::vector<std::uint32_t> out;
    for(const auto& value:names(text)){
        std::size_t used{};const auto number=std::stoul(value,&used,16);
        if(used!=value.size()||number==0||number>0xFFFFFF)throw std::runtime_error("Effects/Armors must contain plugin-local hexadecimal FormIDs");
        if(std::find(out.begin(),out.end(),number)!=out.end())throw std::runtime_error("Duplicate wing registration FormID");
        out.push_back(static_cast<std::uint32_t>(number));
    }
    return out;
}
float value(const std::filesystem::path& path,const char* section,const char* key,float fallback){
    const auto text=read(path,section,key);if(text.empty())return fallback;
    std::size_t used{};const float result=std::stof(text,&used);
    if(used!=text.size()||!std::isfinite(result))throw std::runtime_error(std::string("Invalid wing setting: ")+section+"/"+key);
    return result;
}
// Comma-separated per-pair values; missing trailing entries keep the fallback.
template<std::size_t N>void pairValues(const std::filesystem::path& path,const char* key,std::array<float,N>& out){
    const auto list=names(read(path,"Animation",key));
    if(list.size()>N)throw std::runtime_error(std::string("Too many wing pair values: Animation/")+key);
    for(std::size_t i=0;i<list.size();++i){
        std::size_t used{};const float result=std::stof(list[i],&used);
        if(used!=list[i].size()||!std::isfinite(result))throw std::runtime_error(std::string("Invalid wing setting: Animation/")+key);
        out[i]=result;
    }
}
template<std::size_t N>std::string pairText(const std::array<float,N>& values,std::size_t count){
    std::ostringstream out;out<<std::fixed<<std::setprecision(4);
    for(std::size_t i=0;i<std::min(count,N);++i){if(i)out<<',';out<<values[i];}
    return out.str();
}
template<class T>std::string join(const std::vector<T>& values){
    std::ostringstream out;bool first=true;
    for(const auto& v:values){if(!first)out<<',';first=false;
        if constexpr(std::is_same_v<T,std::string>)out<<v;
        else out<<std::uppercase<<std::hex<<std::setw(6)<<std::setfill('0')<<v;}
    return out.str();
}
}
void defaultWingRegistrationSounds(WingProfile& p,bool awuAvailable){
    p.soundPlugin=awuAvailable?"Animated Wings Ultimate.esp":"Dawnguard.esm";
    p.flapSoundID=awuAvailable?0xC6B:0x19D7F;
    p.fastSoundID=awuAvailable?0xC6C:0x19D7F;
    p.spreadSoundID=awuAvailable?0xC6A:0;
    p.flapSoundPhase=awuAvailable?.1894737f:.08f;p.fastSoundPhase=.2f;
    p.spreadSoundPhase1=.25f;p.spreadSoundPhase2=.75f;p.spreadSoundVolume=1;
}
WingProfile readWingRegistration(const std::filesystem::path& file){
    const auto path=std::filesystem::absolute(file);
    if(std::filesystem::file_size(path)>1024*1024)throw std::runtime_error("Wing registration exceeds 1 MiB");
    WingProfile p;p.id=path.stem().string();p.registrationFile=path.string();
    p.plugin=read(path,"Wings","Plugin");const auto kind=read(path,"Wings","Template");p.wingTemplate=channel::parseTemplate(kind);
    if(p.wingTemplate==channel::Template::legacy)throw std::runtime_error("Template must be AWU, Authored, Fairy, VampireLord or Invisible");
    if(p.plugin.empty()||p.plugin.find_first_of("/\\\r\n=[];")!=p.plugin.npos)throw std::runtime_error("Plugin must be an ESP/ESM/ESL filename");
    p.effectIDs=ids(read(path,"Wings","Effects"));p.armorIDs=ids(read(path,"Wings","Armors"));
    if(p.effectIDs.empty()&&p.armorIDs.empty())throw std::runtime_error("Wing registration requires Effects or Armors");
    p.continuousFlight=p.wingTemplate==channel::Template::fairy;
    p.animationless=p.wingTemplate==channel::Template::invisible;
    p.continuousRiseSpeed=value(path,"Wings","RiseSpeed",4);
    if(p.continuousRiseSpeed<.1f||p.continuousRiseSpeed>100)throw std::runtime_error("RiseSpeed must be .1..100");
    p.rootNode=read(path,"Model","RootNode");p.leftRoots=names(read(path,"Model","LeftRoots"));p.rightRoots=names(read(path,"Model","RightRoots"));
    p.rootNode=channel::stableWingNodeName(p.rootNode);
    for(auto& name:p.leftRoots)name=channel::stableWingNodeName(name);
    for(auto& name:p.rightRoots)name=channel::stableWingNodeName(name);
    if(p.leftRoots.size()!=p.rightRoots.size()||p.leftRoots.size()>8)throw std::runtime_error("LeftRoots and RightRoots must have equal counts, up to 8 pairs");
    auto allRoots=p.leftRoots;allRoots.insert(allRoots.end(),p.rightRoots.begin(),p.rightRoots.end());std::sort(allRoots.begin(),allRoots.end());
    if(std::adjacent_find(allRoots.begin(),allRoots.end())!=allRoots.end())throw std::runtime_error("Wing roots must have distinct names");
    if(!channel::storableWingName(p.rootNode))throw std::runtime_error("Invalid RootNode name");
    // Authored: the model's own embedded sequences. A missing fast or spread
    // sequence reuses the previous channel's sequence.
    p.flapSequence=read(path,"Model","PowerSequence","mAnimation4_Loop");
    p.glideSequence=read(path,"Model","FastSequence","mAnimation4s");
    p.spreadSequence=read(path,"Model","SpreadSequence","mAnimation1_Loop");
    for(const auto* name:{&p.flapSequence,&p.glideSequence,&p.spreadSequence})
        if(!channel::storableWingName(*name))throw std::runtime_error("Invalid authored sequence name");
    if(p.wingTemplate==channel::Template::authored&&p.flapSequence.empty())throw std::runtime_error("Authored requires a PowerSequence");
    p.wingPose=channel::defaults(p.wingTemplate);auto& q=p.wingPose;
    q.upDegrees=value(path,"Animation","UpDegrees",q.upDegrees);q.downDegrees=value(path,"Animation","DownDegrees",q.downDegrees);
    q.forwardTiltDegrees=value(path,"Animation","ForwardTiltDegrees",q.forwardTiltDegrees);
    q.spreadDegrees=value(path,"Animation","SpreadDegrees",q.spreadDegrees);q.fastAmplitude=value(path,"Animation","FastAmplitude",q.fastAmplitude);
    q.flutterDegrees=value(path,"Animation","FlutterDegrees",q.flutterDegrees);q.flutterPeriod=value(path,"Animation","FlutterPeriodSeconds",q.flutterPeriod);
    q.jointFactor=value(path,"Animation","JointFactor",q.jointFactor);q.jointDelay=value(path,"Animation","JointDelaySeconds",q.jointDelay);
    q.flowStrength=value(path,"Animation","FlowStrength",q.flowStrength);q.tipDelaySeconds=value(path,"Animation","TipDelaySeconds",q.tipDelaySeconds);
    q.nativeSwayWeight=value(path,"Animation","NativeSwayWeight",q.nativeSwayWeight);q.blendSeconds=value(path,"Animation","BlendSeconds",q.blendSeconds);
    // Missing articulation keys keep earlier registrations' single-hinge motion.
    // Authored postdates articulation, so its procedural fallback keeps the defaults.
    const bool neutral=p.wingTemplate!=channel::Template::authored;
    q.bottomHold=value(path,"Animation","BottomHold",neutral?0:q.bottomHold);q.foldDegrees=value(path,"Animation","FoldDegrees",neutral?0:q.foldDegrees);
    q.foldCenter=value(path,"Animation","FoldCenter",neutral?.5f:q.foldCenter);q.featherDegrees=value(path,"Animation","FeatherDegrees",neutral?0:q.featherDegrees);
    q.strokeBendDegrees=value(path,"Animation","StrokeBendDegrees",neutral?0:q.strokeBendDegrees);
    // Earlier stroke-bend files shared FoldCenter; keep their appearance.
    q.strokeBendCenter=value(path,"Animation","StrokeBendCenter",neutral?q.foldCenter:q.strokeBendCenter);
    q.pairDelaySeconds.fill(0);q.pairScale.fill(1);
    pairValues(path,"PairDelaySeconds",q.pairDelaySeconds);pairValues(path,"PairScales",q.pairScale);
    channel::setAxisAngles(q,value(path,"Animation","AxisHeadingDegrees",0),value(path,"Animation","AxisTiltDegrees",0));
    if(!channel::valid(q))throw std::runtime_error("Wing animation values are outside their supported ranges; axis must be nonzero");
    p.soundPlugin=read(path,"Sound","Plugin","Dawnguard.esm");
    auto soundID=[&](const char* key,const char* fallback){const auto text=read(path,"Sound",key,fallback);
        if(text=="000000"||text=="0")return 0u;const auto list=ids(text);
        if(list.size()!=1)throw std::runtime_error("Sound IDs must contain one FormID or 000000");return list.front();};
    p.flapSoundID=soundID("PowerStroke","019D7F");
    p.fastSoundID=soundID("FastFlap",p.soundPlugin=="Dawnguard.esm"?"019D7F":"000000");
    p.spreadSoundID=soundID("SpreadFlutter","000000");
    p.flapSoundPhase=value(path,"Sound","MarkerPhase",.08f);p.fastSoundPhase=value(path,"Sound","FastMarkerPhase",.2f);
    p.spreadSoundPhase1=value(path,"Sound","SpreadMarkerPhase1",.25f);p.spreadSoundPhase2=value(path,"Sound","SpreadMarkerPhase2",.75f);
    p.spreadSoundVolume=value(path,"Sound","SpreadVolume",1);
    const auto down=value(path,"Timing","DownstrokeEnd",p.timing.downstrokeEnd);
    if(down<.1f||down>.7f)throw std::runtime_error("DownstrokeEnd must be .1.. .7");p.timing.downstrokeEnd=down;
    if(p.flapSoundPhase<0||p.flapSoundPhase>=down||p.fastSoundPhase<0||p.fastSoundPhase>=1
        ||p.spreadSoundPhase1<0||p.spreadSoundPhase2<=p.spreadSoundPhase1||p.spreadSoundPhase2>=1||p.spreadSoundVolume<0||p.spreadSoundVolume>1)
        throw std::runtime_error("Sound marker phase or volume is out of range");
    return p;
}
std::string wingRegistrationText(const WingProfile& p){
    if(p.wingTemplate==channel::Template::legacy||!channel::valid(p.wingPose))throw std::runtime_error("Cannot save invalid wing template");
    std::ostringstream out;out<<std::fixed<<std::setprecision(4);
    out<<"; Persistent Wingflight registration. Unequipping never deletes this file.\n"
       <<"; Forms are hexadecimal IDs local to Plugin; Effects and Armors are alternative triggers.\n"
       <<"[Wings]\nPlugin="<<p.plugin<<"\nEffects="<<join(p.effectIDs)<<"\nArmors="<<join(p.armorIDs)
       <<"\nTemplate="<<channel::templateName(p.wingTemplate)<<"\nRiseSpeed="<<p.continuousRiseSpeed<<"\n\n"
       <<"; Only these wing branches are driven. Empty mappings request structural detection.\n"
       <<"[Model]\nRootNode="<<channel::stableWingNodeName(p.rootNode)<<"\nLeftRoots=";
    auto stableLeft=p.leftRoots,stableRight=p.rightRoots;
    for(auto& name:stableLeft)name=channel::stableWingNodeName(name);
    for(auto& name:stableRight)name=channel::stableWingNodeName(name);
    out<<join(stableLeft)<<"\nRightRoots="<<join(stableRight)<<'\n';
    if(p.wingTemplate==channel::Template::authored)
        out<<"PowerSequence="<<p.flapSequence<<"\nFastSequence="<<p.glideSequence<<"\nSpreadSequence="<<p.spreadSequence<<'\n';
    out<<'\n';
    const auto& q=p.wingPose;
    const auto axis=channel::storedAxisAngles(q);
    out<<"; Degrees use the runtime reference pose. Heading/tilt follows the wing attachment.\n"
       <<"[Animation]\nForwardTiltDegrees="<<q.forwardTiltDegrees<<"\nUpDegrees="<<q.upDegrees<<"\nDownDegrees="<<q.downDegrees<<"\nSpreadDegrees="<<q.spreadDegrees
       <<"\nFastAmplitude="<<q.fastAmplitude<<"\nFlutterDegrees="<<q.flutterDegrees<<"\nFlutterPeriodSeconds="<<q.flutterPeriod
       <<"\nFlowStrength="<<q.flowStrength<<"\nTipDelaySeconds="<<q.tipDelaySeconds
       <<"\nJointFactor="<<q.jointFactor<<"\nJointDelaySeconds="<<q.jointDelay<<"\nNativeSwayWeight="<<q.nativeSwayWeight
       <<"\nBlendSeconds="<<q.blendSeconds
       <<"\nBottomHold="<<q.bottomHold<<"\nFoldDegrees="<<q.foldDegrees<<"\nFoldCenter="<<q.foldCenter<<"\nFeatherDegrees="<<q.featherDegrees<<"\nStrokeBendDegrees="<<q.strokeBendDegrees<<"\nStrokeBendCenter="<<q.strokeBendCenter;
    // One entry per configured pair, and never drop a value that was changed.
    auto pairs=std::max<std::size_t>(p.leftRoots.size(),1);
    for(std::size_t i=0;i<q.pairScale.size();++i)if(q.pairDelaySeconds[i]!=0||q.pairScale[i]!=1)pairs=std::max(pairs,i+1);
    out<<"\nPairDelaySeconds="<<pairText(q.pairDelaySeconds,pairs)<<"\nPairScales="<<pairText(q.pairScale,pairs)
       <<"\nAxisHeadingDegrees="<<axis[0]<<"\nAxisTiltDegrees="<<axis[1]<<'\n';
    out<<"\n[Timing]\nDownstrokeEnd="<<p.timing.downstrokeEnd<<"\n\n"
       <<"[Sound]\nPlugin="<<p.soundPlugin<<"\nPowerStroke=";
    out<<std::uppercase<<std::hex<<std::setw(6)<<std::setfill('0')<<p.flapSoundID<<"\nFastFlap="<<std::setw(6)<<p.fastSoundID
       <<"\nSpreadFlutter="<<std::setw(6)<<p.spreadSoundID<<std::dec<<"\nMarkerPhase="<<p.flapSoundPhase<<"\nFastMarkerPhase="<<p.fastSoundPhase
       <<"\nSpreadMarkerPhase1="<<p.spreadSoundPhase1<<"\nSpreadMarkerPhase2="<<p.spreadSoundPhase2<<"\nSpreadVolume="<<p.spreadSoundVolume<<"\n";
    return out.str();
}
}
