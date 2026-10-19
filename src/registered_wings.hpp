#pragma once
#include "config.hpp"
#include <RE/Skyrim.h>
#include <memory>

namespace wingflight {
struct WingMatch;
struct WingPlayback;
struct WingCandidate {
    WingProfile profile;
    std::string name,reason;
    unsigned bones{},pairs{};
    bool compatible{};
    bool authored{}; // The model carries the registration's wingbeat sequences.
};
inline bool registeredWing(const WingProfile* p){return p&&p->wingTemplate!=channel::Template::legacy;}
inline bool largeWingSprint(const WingProfile* p){return p&&(p->nativeVampire||p->wingTemplate==channel::Template::vampireLord);}
// Called on the game task thread. UI only exchanges value snapshots.
void loadWingRegistrations();
WingMatch findRegisteredWings(RE::Actor* actor);
std::vector<WingCandidate> scanWingCandidates(RE::Actor* actor);
std::vector<WingProfile> wingRegistrationProfiles();
std::string saveWingRegistration(WingProfile profile,bool fromScanner=false);
void applyRegisteredWingPoses(RE::NiAVObject* object,RE::NiUpdateData* data=nullptr,bool postEvaluation=false);
void renderWingRegistrationMenu();

class RegisteredWingRuntime {
public:
    struct State;
    RegisteredWingRuntime();
    ~RegisteredWingRuntime();
    WingPlayback prepare(WingMatch match);
    void advance(float dt);
    bool blend(float flap,float ownership,float fast);
    bool preview(float amount,float period,float amplitude);
    bool parkPower();
    bool parkFast();
    void powerRate(float rate);
    void loopRate(float fast,float spread);
    void pitch(float degrees);
    void release(bool smooth);
private:
    std::shared_ptr<State> state_;
};
}
