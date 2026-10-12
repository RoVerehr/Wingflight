#pragma once
#include "spread_amplitude_policy.hpp"
#include "wing_blend.hpp"
#include "diagnostics.hpp"
#include <array>
#include <limits>

namespace wingflight::visual {
struct Clock {
    float phase{}, duration{1}, rate{1};
    bool resetHidden(float weight,float start=0) {
        if(weight!=0 || !std::isfinite(start) || start<0 || start>=1)return false;
        phase=start;return true;
    }
    void advance(float dt) {
        if(!std::isfinite(dt)||dt<=0||!std::isfinite(duration)||duration<=0
            ||!std::isfinite(rate)||rate<0){
            if(!std::isfinite(dt)||dt<0||!std::isfinite(duration)||duration<=0||!std::isfinite(rate)||rate<0)
                diagnostic::report("wing.clock-invalid","skip clock update",this,
                    !std::isfinite(dt)||dt<0?"delta":!std::isfinite(duration)||duration<=0?"duration":"rate",
                    !std::isfinite(dt)||dt<0?dt:!std::isfinite(duration)||duration<=0?duration:rate);
            return;}
        phase=std::fmod(phase+std::min(dt,0.1f)*rate/duration,1.0f);
    }
};
struct Sample {
    float x{},y{},z{};
    float w{1},qx{},qy{},qz{};
    float scale{1};
};
static_assert(sizeof(Sample)==0x20);
inline bool channel(float f){return std::isfinite(f)&&f!=-std::numeric_limits<float>::max();}
inline Sample validSample(Sample value,const Sample& reference,const void* context=nullptr) {
    if(!channel(value.x)||!channel(value.y)||!channel(value.z)){
        // Ni uses -FLT_MAX for an intentionally unkeyed channel.
        if(!std::isfinite(value.x)||!std::isfinite(value.y)||!std::isfinite(value.z))
            diagnostic::report("sample.translation-invalid","use reference translation",context,
                !std::isfinite(value.x)?"x":!std::isfinite(value.y)?"y":"z",
                !std::isfinite(value.x)?value.x:!std::isfinite(value.y)?value.y:value.z);
        value.x=reference.x;value.y=reference.y;value.z=reference.z;
    }
    if(!flutter::valid({value.qx,value.qy,value.qz,value.w})){
        if(value.w!=-std::numeric_limits<float>::max())
            diagnostic::report("sample.rotation-invalid","use reference rotation",context,"quaternion squared length",
                value.w*value.w+value.qx*value.qx+value.qy*value.qy+value.qz*value.qz,1);
        value.w=reference.w;value.qx=reference.qx;value.qy=reference.qy;value.qz=reference.qz;
    }
    if(!channel(value.scale)||value.scale<=0){
        if(value.scale!=-std::numeric_limits<float>::max())diagnostic::report("sample.scale-invalid","use reference scale",context,{},value.scale,reference.scale);
        value.scale=reference.scale;}
    return value;
}
// Normalize a weighted quaternion sum as Ni's ordinary transform blender does.
// Align hemispheres to the dominant contributor before summation.
inline Sample combine(const std::array<Sample,4>& poses,const std::array<float,4>& weights) {
    std::size_t dominant=0;float total=0;
    for(std::size_t i=0;i<4;++i){if(weights[i]>weights[dominant])dominant=i;total+=weights[i];}
    if(!std::isfinite(total)||total<=0){
        diagnostic::report("wing.weight-total-invalid","use original pose",nullptr,{},total);return poses[0];}
    const auto& anchor=poses[dominant];Sample result{};result.w=0;result.scale=0;
    for(std::size_t i=0;i<4;++i){
        const auto& p=poses[i];const float weight=weights[i]/total;
        const float sign=anchor.w*p.w+anchor.qx*p.qx+anchor.qy*p.qy+anchor.qz*p.qz<0?-1.0f:1.0f;
        result.x+=p.x*weight;result.y+=p.y*weight;result.z+=p.z*weight;result.scale+=p.scale*weight;
        result.w+=p.w*weight*sign;result.qx+=p.qx*weight*sign;result.qy+=p.qy*weight*sign;result.qz+=p.qz*weight*sign;
    }
    const auto q=legs::normalized({result.qx,result.qy,result.qz,result.w});
    result.w=q.w;result.qx=q.x;result.qy=q.y;result.qz=q.z;return result;
}
}
