#pragma once
#include <string_view>
namespace wingflight {
inline bool walkingFootEvent(std::string_view event){
    return event=="FootLeft"||event=="FootRight"||event=="FootLeft2"||event=="FootRight2"
        ||event=="FootLeft3"||event=="FootRight3"||event=="FootScuffLeft"||event=="FootScuffRight"
        ||event=="FootSprintLeft"||event=="FootSprintRight";
}
inline bool suppressFlightFootstep(bool enabled,bool fullFlight,bool player,bool airborne,std::string_view event){
    return enabled&&fullFlight&&player&&airborne&&walkingFootEvent(event);
}
inline bool footContactEvent(std::string_view event){
    return walkingFootEvent(event)||event=="FootFront"||event=="FootBack"||event=="FootLand";
}
inline std::string_view directSoundName(std::string_view event,std::string_view payload){
    if(event=="SoundPlay")return payload;
    if(event.starts_with("SoundPlay."))return event.substr(10);
    return {};
}
inline bool airborneFootAudio(bool enabled,bool player,bool airborne,std::string_view event,
    std::string_view payload={},bool registeredFootTag=false,bool registeredFootSound=false){
    const auto sound=directSoundName(event,payload);
    return enabled&&player&&airborne&&(footContactEvent(event)||registeredFootTag
        ||(!sound.empty()&&(sound.starts_with("FST")||registeredFootSound)));
}
}
