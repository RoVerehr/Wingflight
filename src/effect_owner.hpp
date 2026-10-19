#pragma once
#include "diagnostics.hpp"
#include "actor_state.hpp"

namespace wingflight {
// expectedOwner must come from an Actor API, never from casting Effect::target.
// MagicTarget is an interior Actor subobject (offset 0xA0 in runtime 1.6.1170).
template <class Actor, class Effect>
Actor* wingEventActor(Actor* expectedOwner, Effect* effect) {
    if (!expectedOwner || !effect || effect->target != expectedOwner->AsMagicTarget()) {
        if(expectedOwner&&effect)diagnostic::report("wing.effect-owner-mismatch","ignore effect for this actor",effect);
        return nullptr; }
    if (expectedOwner->IsDead() || knockedDown(expectedOwner) || !expectedOwner->Is3DLoaded()) { return nullptr; }
    return expectedOwner;
}
// Registered equipment and manual Invisible rules can be bound without a magic
// effect. Effect-backed rules still require the same complete-Actor ownership.
template <class Actor, class Effect>
Actor* wingSoundActor(Actor* expectedOwner,Effect* effect,bool allowsEffectless){
    if(effect)return wingEventActor(expectedOwner,effect);
    if(!allowsEffectless||!expectedOwner||expectedOwner->IsDead()||knockedDown(expectedOwner)||!expectedOwner->Is3DLoaded())return nullptr;
    return expectedOwner;
}
}
