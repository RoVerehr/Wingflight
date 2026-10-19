#pragma once

namespace wingflight {
// Knocked down, staggered to the ground, paralyzed or ragdolled by the engine's
// own knock state machine. Unlike the IsInRagdollState() call, this is a plain
// actor-state read, so physics/animation mods that flag an upright, flying
// actor as ragdolled do not end flight or hide its wings. KNOCK_STATE_ENUM
// kNormal is zero.
// Accepts raw actor pointers and NiPointer handles alike.
template<class ActorPointer> bool knockedDown(const ActorPointer& actor){
    const auto state=actor?actor->AsActorState():nullptr;
    using Knock=decltype(state->GetKnockState());
    return !state||state->GetKnockState()!=Knock{};
}
}
