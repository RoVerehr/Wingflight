#pragma once
#include <algorithm>

namespace wingflight::aerial {
// Spawn's return ABI is deliberately irrelevant. Adopt only an actual new
// model obtained through the engine's owned effect list, filtered by owner/art.
template<class Snapshot,class Spawn>
auto createOwnedTemporaryWing(Snapshot snapshot,Spawn spawn) {
    const auto before=snapshot();
    (void)spawn();
    const auto after=snapshot();
    using Model=typename decltype(after)::value_type;
    Model created{};
    for(const auto& model:after){
        if(!model||std::find(before.begin(),before.end(),model)!=before.end()||model==created)continue;
        if(created)return Model{}; // Ambiguous ownership: finite effects expire.
        created=model;
    }
    return created;
}
}
