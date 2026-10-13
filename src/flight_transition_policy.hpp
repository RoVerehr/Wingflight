#pragma once
#include <string_view>
namespace wingflight::pose {
struct TransitionTiming { float movement{0.75f},sprintIn{0.75f},sprintOut{0.75f}; };
// Names audited against mt_behavior. Restrict sprint changes to the ordinary
// unarmed locomotion pair, not sneak rolls, attacks or perspective transitions.
inline float transitionSeconds(std::string_view effect,std::string_view from,std::string_view to,const TransitionTiming& timing){
    if(effect=="moveStartTransition"||effect=="moveStopTransition")return timing.movement;
    if(effect=="SprintSyncBlendTransition"){
        if(to=="SprintBFR"&&from=="Default_Locomotion_MSG")return timing.sprintIn;
        if(from=="SprintBFR"&&to=="Default_Locomotion_MSG")return timing.sprintOut;
    }
    return 0;
}
}
