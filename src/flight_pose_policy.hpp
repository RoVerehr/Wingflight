#pragma once
#include <array>
#include <string>
#include <string_view>
namespace wingflight::pose {
inline constexpr std::array<std::string_view,3> slots{"animations/hover.hkx","animations/forwardflight.hkx","animations/fastflight.hkx"};
inline constexpr std::array<std::string_view,3> defaults{
    "animations/openanimationreplacer/wingflight flight poses/hover/hover.hkx",
    "animations/openanimationreplacer/wingflight flight poses/forward flight/forwardflight.hkx",
    "animations/openanimationreplacer/wingflight flight poses/fast flight/fastflight.hkx"};
inline constexpr std::array<std::string_view,3> resources{
    "animations/openanimationreplacer/flyinganimationresource/animations/male/mt_idle.hkx",
    "animations/openanimationreplacer/flyinganimationresource/animations/male/mt_runforward.hkx",
    "animations/openanimationreplacer/flyinganimationresource/animations/male/mt_sprintforward.hkx"};
// Each optional local Default is independent. OAR still chooses conditional
// replacements against slots, regardless of the physical fallback selected here.
template<class Exists>
inline std::array<std::string_view,3> resolveDefaults(Exists exists){
    std::array<std::string_view,3> paths{};
    for(unsigned mode=0;mode<paths.size();++mode){
        if(exists(defaults[mode]))paths[mode]=defaults[mode];
        else if(exists(resources[mode]))paths[mode]=resources[mode];
    }
    if(paths[2].empty())paths[2]=paths[1];
    return paths;
}
inline constexpr std::array modes{1,2,3};
inline constexpr unsigned modeIndex(int mode){return mode==3?2:mode==2?1:0;}
// The graph chooses the native node. Routing is fixed for that node's lifetime,
// including the outgoing side of a native blend; changing the global flight
// state must never convert both sides of the transition to the incoming pose.
inline int nativeMode(std::string_view path){
    if(!path.starts_with("animations/"))return -1;
    path.remove_prefix(11);
    if(path.starts_with("male/"))path.remove_prefix(5);
    else if(path.starts_with("female/"))path.remove_prefix(7);
    if(path=="mt_idle.hkx"||path=="mt_turnleft60.hkx"||path=="mt_turnright60.hkx"
        ||path=="mt_turnleft180.hkx"||path=="mt_turnright180.hkx")return 0;
    if(path=="mt_sprintforward.hkx")return 2;
    constexpr std::array directions{"forward","backward","left","right","forwardleft","forwardright","backwardleft","backwardright"};
    for(auto direction:directions)
        if(path==std::string("mt_walk")+direction+".hkx"||path==std::string("mt_run")+direction+".hkx")return 1;
    return -1;
}
inline int graphMode(float speed,bool sprinting){return sprinting?3:speed>0.1f?2:1;}
inline constexpr unsigned fallbackIndex(unsigned mode,bool fastPresent){return mode==2&&!fastPresent?1:mode;}
inline bool locomotion(std::string_view path){
    if(path.starts_with("animations/male/")||path.starts_with("animations/female/"))path.remove_prefix(path.find('/',11)+1);
    else if(path.starts_with("animations/"))path.remove_prefix(11);
    else return false;
    constexpr std::array names{
        "mt_idle.hkx","mt_walkforward.hkx","mt_walkbackward.hkx","mt_walkleft.hkx","mt_walkright.hkx",
        "mt_walkforwardleft.hkx","mt_walkforwardright.hkx","mt_walkbackwardleft.hkx","mt_walkbackwardright.hkx",
        "mt_runforward.hkx","mt_runbackward.hkx","mt_runleft.hkx","mt_runright.hkx",
        "mt_runforwardleft.hkx","mt_runforwardright.hkx","mt_runbackwardleft.hkx","mt_runbackwardright.hkx",
        "mt_sprintforward.hkx","mt_turnleft60.hkx","mt_turnright60.hkx","mt_turnleft180.hkx","mt_turnright180.hkx",
        "mt_jump.hkx","mt_jumpfast.hkx","mt_jumpfastleft.hkx","mt_jumpfastright.hkx",
        "mt_jumpfall.hkx","mt_jumpfallleft.hkx","mt_jumpfallright.hkx","mt_jumpland.hkx","mt_jumplandleft.hkx","mt_jumplandright.hkx"};
    for(auto n:names)if(n==path)return true;
    return false;
}
inline bool binding(std::string_view path,unsigned mode){
    if(mode>=slots.size())return false;
    constexpr std::string_view dataPrefix="data/meshes/actors/character/";
    if(path.starts_with(dataPrefix))path.remove_prefix(dataPrefix.size());
    if(path==slots[mode]||path==defaults[mode]||path==resources[mode])return true;
    constexpr std::string_view prefix="animations/openanimationreplacer/";
    const auto oar=path.find(prefix);
    if(oar==std::string_view::npos)return false;
    path.remove_prefix(oar+prefix.size());
    // Standard OAR mod / condition submod / filename. Directory names are free.
    for(unsigned level=0;level<2;++level){const auto slash=path.find('/');if(slash==std::string_view::npos)return false;path.remove_prefix(slash+1);}
    if(path==slots[mode].substr(11))return true;
    const std::array<std::string_view,3> variants{"_variants_hover","_variants_forwardflight","_variants_fastflight"};
    return path==variants[mode]||(path.starts_with(variants[mode])&&path.size()>variants[mode].size()&&path[variants[mode].size()]=='/');
}
inline bool acceptedBinding(std::string_view physicalPath,std::string_view replacementPath,unsigned mode){
    // OAR metadata identifies the selected logical slot even when hashes merge
    // physical bindings with FAR or with a different pose slot.
    return replacementPath.empty()?binding(physicalPath,mode):binding(replacementPath,mode);
}
// Native graph clones can also carry 0xC. Only an untracked, out-of-graph
// copy may be ignored as an OAR outgoing blend; the marker alone is ambiguous.
inline constexpr bool ignoreTransitionCopy(bool marker,bool tracked,bool graphOwned){return marker&&!tracked&&!graphOwned;}
inline bool allowed(bool full,bool armed,bool humanoid,bool safe,bool supported,bool enabled){return full&&!armed&&humanoid&&safe&&!supported&&enabled;}
inline bool project(std::string_view path){return path=="actors/character"||path=="actors/character/";}
inline bool graph(std::string_view name){return name.find("firstperson")==std::string_view::npos&&name.find("1stperson")==std::string_view::npos;}
inline bool canAppend(unsigned size){return size<0xff00;}
}
