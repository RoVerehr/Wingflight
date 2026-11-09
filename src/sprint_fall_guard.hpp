#pragma once
#include <cstdint>
#include <span>
namespace RE {class Actor;}
namespace wingflight {
using SprintFallEligibility=bool(*)(RE::Actor*);
void installSprintFallGuard(SprintFallEligibility eligible);
inline bool sprintFallingResult(bool native,bool owned,std::uintptr_t caller,std::span<const std::uintptr_t> allowed){
    if(native&&owned)for(auto site:allowed)if(site&&site==caller)return false;
    return native;
}
}
