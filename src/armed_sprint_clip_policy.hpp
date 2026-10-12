#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace wingflight {
struct SprintIdleSlot { std::string_view sprint,idle; };
// Canonical third-person locomotion paths, not attacks, dodges or first-person
// arm overlays. OAR evaluates the idle slot using its normal priority/conditions.
inline constexpr std::array sprintIdleSlots{
    SprintIdleSlot{"animations/male/mt_sprintforward.hkx","animations/male/mt_idle.hkx"},
    SprintIdleSlot{"animations/female/mt_sprintforward.hkx","animations/female/mt_idle.hkx"},
    SprintIdleSlot{"animations/mt_sprintforward.hkx","animations/mt_idle.hkx"},
    SprintIdleSlot{"animations/mt_sprintforwardsword.hkx","animations/1hm_idle.hkx"},
    SprintIdleSlot{"animations/2hm_sprintforwardsword.hkx","animations/2hm_idle.hkx"},
    SprintIdleSlot{"animations/2hw_sprintforwardsword.hkx","animations/2hw_idle.hkx"},
    SprintIdleSlot{"animations/dw_sprintforwardsword.hkx","animations/1hm_idle.hkx"},
    SprintIdleSlot{"animations/bow_sprint.hkx","animations/bow_idleheld.hkx"},
    SprintIdleSlot{"animations/dlc01/crossbowsprintforward.hkx","animations/dlc01/crossbow_idleheld.hkx"},
    SprintIdleSlot{"animations/magic_sprintforward.hkx","animations/1hm_idle.hkx"},
    SprintIdleSlot{"animations/staffrightleft_sprint.hkx","animations/staff_idle.hkx"},
    SprintIdleSlot{"animations/torch_sprintforward.hkx","animations/tor_idle.hkx"}
};
inline bool thirdPersonIdleProject(std::string_view canonicalProject,bool maleIdle,bool femaleIdle,bool flatIdle){
    // Gender belongs to the character project, not the shared behavior XML.
    // FirstPerson has weapon idle/sprint slots too and must remain excluded.
    return canonicalProject.find("firstperson")==std::string_view::npos
        &&canonicalProject.find("1stperson")==std::string_view::npos
        &&(maleIdle||femaleIdle||flatIdle);
}
inline std::array<std::string_view,3> nativeSlotCandidates(std::string_view path){
    if(path=="animations/male/mt_idle.hkx")return {path,"animations/female/mt_idle.hkx","animations/mt_idle.hkx"};
    if(path=="animations/female/mt_idle.hkx")return {path,"animations/male/mt_idle.hkx","animations/mt_idle.hkx"};
    if(path=="animations/mt_idle.hkx")return {path,"animations/female/mt_idle.hkx","animations/male/mt_idle.hkx"};
    if(path=="animations/male/mt_sprintforward.hkx")return {path,"animations/female/mt_sprintforward.hkx","animations/mt_sprintforward.hkx"};
    if(path=="animations/female/mt_sprintforward.hkx")return {path,"animations/male/mt_sprintforward.hkx","animations/mt_sprintforward.hkx"};
    if(path=="animations/mt_sprintforward.hkx")return {path,"animations/female/mt_sprintforward.hkx","animations/male/mt_sprintforward.hkx"};
    return {path,{},{}};
}
template<class NameAt>
std::uint16_t resolveNativeIdleSlot(std::uint32_t count,NameAt nameAt,std::string_view path){
    for(const auto candidate:nativeSlotCandidates(path)){
        if(candidate.empty())continue;
        for(std::uint32_t i=0;i<count&&i<0xffff;++i)
            if(nameAt(i)==candidate)return static_cast<std::uint16_t>(i);
    }
    return 0xffff;
}
inline std::string_view sprintIdleSlot(std::string_view canonicalPath){
    for(const auto& slot:sprintIdleSlots)if(slot.sprint==canonicalPath)return slot.idle;
    return {};
}
// OAR's temporary blending copies use userData 0xC. Do not redirect or restart
// those, or a retained outgoing pose could become the incoming idle abruptly.
inline bool routeSprintIdle(bool playerThirdPerson,bool armedIdle,bool supportedSlot,bool blendingCopy){
    return playerThirdPerson&&armedIdle&&supportedSlot&&!blendingCopy;
}
inline bool restartSprintClip(bool aliased,bool wanted,bool active,bool blendingCopy){
    return active&&!blendingCopy&&aliased!=wanted;
}
inline bool idleSlotBinding(std::string_view canonicalPath,std::string_view idleSlot){
    const auto slash=idleSlot.find_last_of('/');const auto file=idleSlot.substr(slash==std::string_view::npos?0:slash+1);
    const auto last=canonicalPath.find_last_of('/');
    if(canonicalPath.substr(last==std::string_view::npos?0:last+1)==file)return true;
    const auto stem=file.substr(0,file.ends_with(".hkx")?file.size()-4:file.size());
    while(!canonicalPath.empty()){
        const auto end=canonicalPath.find('/');const auto segment=canonicalPath.substr(0,end);
        if(segment.starts_with("_variants_")){
            const auto slot=segment.substr(10);if(slot==stem||slot==file)return true;
        }
        if(end==std::string_view::npos)break;canonicalPath.remove_prefix(end+1);
    }
    return false;
}
inline bool repairSprintBinding(bool wanted,bool aliased,bool correctBinding,bool active,bool blendingCopy){
    return wanted&&aliased&&!correctBinding&&active&&!blendingCopy;
}
}
