#pragma once
#include <cstdint>
namespace wingflight::vampire {
constexpr bool useInvisibleFallback(bool automatic,bool visibleEffect,bool visibleSkeleton,bool invisibleEffect) {
    return automatic&&!visibleEffect&&!visibleSkeleton&&invisibleEffect;
}
enum class Driver : std::uint8_t { automatic, skeleton, effect, unavailable };
inline Driver chooseDriver(Driver requested,bool effectVisible,bool skeletonVisible){
    if(requested==Driver::effect)return effectVisible?Driver::effect:Driver::unavailable;
    if(requested==Driver::skeleton)return skeletonVisible?Driver::skeleton:Driver::unavailable;
    if(effectVisible)return Driver::effect;
    return skeletonVisible?Driver::skeleton:Driver::unavailable;
}
inline bool rigVisible(bool vlrn,bool beastAppearance,bool selectedNativeWingWorn){
    return !vlrn||beastAppearance||selectedNativeWingWorn;
}
inline bool validBody(bool knownTransformation,bool nativeProject,bool humanProject){
    return knownTransformation&&(nativeProject||humanProject);
}
}
