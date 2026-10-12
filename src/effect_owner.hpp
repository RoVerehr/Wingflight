#pragma once
#include "diagnostics.hpp"

namespace wingflight {
// expectedOwner must come from an Actor API, never from casting Effect::target.
// MagicTarget is an interior Actor subobject (offset 0xA0 in runtime 1.6.1170).
template <class Actor, class Effect>
Actor* wingEventActor(Actor* expectedOwner, Effect* effect) {
    if (!expectedOwner || !effect || effect->target != expectedOwner->AsMagicTarget()) {
        if(expectedOwner&&effect)diagnostic::report("wing.effect-owner-mismatch","ignore effect for this actor",effect);
        return nullptr; }
    if (expectedOwner->IsDead() || expectedOwner->IsInRagdollState() || !expectedOwner->Is3DLoaded()) { return nullptr; }
    return expectedOwner;
}
}
