#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace wingflight::runtime {
struct SceneCall {
    unsigned size{};
    std::uintptr_t target{};
    bool indirect{};
};
// Keep the current target, including another plugin's generated relay. An
// indirect CALL describes a pointer slot; the caller must read that slot first.
inline SceneCall inspectSceneCall(std::uintptr_t site,std::span<const std::uint8_t> bytes) {
    if(bytes.size()>=5&&bytes[0]==0xE8) {
        std::int32_t displacement{};std::memcpy(&displacement,bytes.data()+1,4);
        return {5,site+5+displacement,false};
    }
    if(bytes.size()>=6&&bytes[0]==0xFF&&bytes[1]==0x15) {
        std::int32_t displacement{};std::memcpy(&displacement,bytes.data()+2,4);
        return {6,site+6+displacement,true};
    }
    return {};
}
}
