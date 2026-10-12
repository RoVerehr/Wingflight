#pragma once
#include "lower_body_policy.hpp"
#include <span>

namespace wingflight::flutter {
using legs::Quaternion;
inline bool valid(Quaternion q){
    const float n=legs::dot(q,q);return std::isfinite(n)&&n>0.8f&&n<1.2f;
}
inline Quaternion center(std::span<const Quaternion> keys){
    if(keys.empty())return {};
    Quaternion sum{0,0,0,0};const auto first=legs::normalized(keys.front());
    // A duplicated closing sample must not bias the center toward that phase.
    const auto count=keys.size()>1&&std::abs(legs::dot(first,legs::normalized(keys.back())))>0.999999f
        ?keys.size()-1:keys.size();
    for(std::size_t i=0;i<count;++i){auto q=legs::normalized(keys[i]);
        if(legs::dot(first,q)<0)q={-q.x,-q.y,-q.z,-q.w};
        sum.x+=q.x;sum.y+=q.y;sum.z+=q.z;sum.w+=q.w;}
    return legs::normalized(sum);
}
inline Quaternion scale(Quaternion anchor,Quaternion sample,float amplitude){
    anchor=legs::normalized(anchor);sample=legs::normalized(sample);
    if(amplitude==1)return sample;
    if(!std::isfinite(amplitude)){
        diagnostic::report("flutter.amplitude-nonfinite","retain original flutter sample",nullptr,{},amplitude);return sample;}
    amplitude=std::clamp(amplitude,0.0f,2.0f);
    float d=legs::dot(anchor,sample);
    if(d<0){sample={-sample.x,-sample.y,-sample.z,-sample.w};d=-d;}
    if(d>0.999999f)return anchor;
    const float angle=std::acos(std::clamp(d,-1.0f,1.0f)),s=std::sin(angle);
    const float a=std::sin((1-amplitude)*angle)/s,b=std::sin(amplitude*angle)/s;
    return legs::normalized({a*anchor.x+b*sample.x,a*anchor.y+b*sample.y,
        a*anchor.z+b*sample.z,a*anchor.w+b*sample.w});
}
}
