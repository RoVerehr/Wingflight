#pragma once
#include <atomic>
#include <string_view>
namespace wingflight {
inline bool useArmedFlightIdle(bool enabled,bool fullFlight,bool armed,bool safeActor,bool supported){
    return enabled&&fullFlight&&armed&&safeActor&&!supported;
}
// SprintStart must reach the graph to establish native sprint state. Only the
// explicit gameplay restriction may consume it; cosmetic idle routing cannot.
inline bool suppressArmedSprintStart(bool forbidSprint,std::string_view event){return forbidSprint&&event=="SprintStart";}
inline bool flightSprintAllowed(bool nativeSprint,bool forbidSprint){return nativeSprint&&!forbidSprint;}
struct ArmedSprintChange { bool enter{},exit{},resumeSprint{}; };
class ArmedSprintLatch {
public:
    ArmedSprintChange update(bool wanted,bool nativeSprint,bool allowResume){
        const bool before=active.exchange(wanted);
        return {wanted&&!before,!wanted&&before,!wanted&&before&&nativeSprint&&allowResume};
    }
    void reset(){active=false;}
private:std::atomic_bool active{};
};
}
