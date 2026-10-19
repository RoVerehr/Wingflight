#pragma once
#include "wing_channel_policy.hpp"
#include "lower_body_policy.hpp"
#include "armed_bank.hpp"

namespace wingflight::channel {
// Convert a configured axis only through the wing's static attachment frame
// and the live descendant chain. Body/camera transforms never enter this map.
template<class Matrix>
inline std::array<float,3> wingParentAxis(const Matrix& parentToAnchor,const Matrix& referenceToAnchor,
    const std::array<float,3>& configured){
    std::array<float,3> inAnchor{},inParent{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)inAnchor[i]+=referenceToAnchor[i][j]*configured[j];
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)inParent[i]+=parentToAnchor[j][i]*inAnchor[j];
    return inParent;
}
// Swing the segment into its incoming direction without erasing authored twist.
// Roots retain their placement; zero-length helpers retain the reference pose.
inline legs::Quaternion extendedWingReference(legs::Quaternion base,
    std::array<float,3> outgoing,std::array<float,3> incoming){
    auto unit=[](std::array<float,3>& v){
        const float n=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        if(!std::isfinite(n)||n<.00001f)return false;
        for(auto& x:v)x/=n;return true;
    };
    if(!unit(outgoing)||!unit(incoming))return base;
    const auto m=legs::matrix(base);std::array<float,3> from{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)from[i]+=m[i][j]*outgoing[j];
    const float d=std::clamp(from[0]*incoming[0]+from[1]*incoming[1]+from[2]*incoming[2],-1.f,1.f);
    if(d>.999999f)return base;
    std::array<float,3> cross{from[1]*incoming[2]-from[2]*incoming[1],
        from[2]*incoming[0]-from[0]*incoming[2],from[0]*incoming[1]-from[1]*incoming[0]};
    if(d<-.999999f){
        // Deterministic perpendicular for a folded-back segment.
        unsigned smallest=0;for(unsigned i=1;i<3;++i)if(std::abs(from[i])<std::abs(from[smallest]))smallest=i;
        std::array<float,3> axis{};axis[smallest]=1;
        cross={from[1]*axis[2]-from[2]*axis[1],from[2]*axis[0]-from[0]*axis[2],from[0]*axis[1]-from[1]*axis[0]};
        unit(cross);return legs::multiply({cross[0],cross[1],cross[2],0},base);
    }
    return legs::multiply(legs::normalized({cross[0],cross[1],cross[2],1+d}),base);
}
// At the raised endpoint the whole chain is extended. Curvature returns
// smoothly through the downstroke and fades during recovery, including fast flaps.
inline float wingBendWeight(Template kind,float powerPhase,float fastPhase,float downEnd,
    float flap,float fast,float spread,float hold=0){
    if(kind==Template::fairy)return 1;
    const float total=flap+fast+spread;
    return total>.0001f?std::clamp((flap*stroke(powerPhase,downEnd,hold)+fast*stroke(fastPhase,downEnd)+spread)/total,0.f,1.f):0.f;
}
// Unwrapped stroke progress, so a pair can follow the same stroke a short time
// later without seeking backwards across the loop seam.
class StrokeProgress {
public:
    void clear(){started_=false;turns_=0;}
    double update(float phase){
        if(!std::isfinite(phase))return value_;
        if(started_&&phase+.5f<last_)++turns_;
        else if(started_&&phase>last_+.5f)--turns_;
        started_=true;last_=phase;value_=turns_+static_cast<double>(phase);return value_;
    }
private:bool started_{};float last_{};double turns_{},value_{};
};
// Same signed body-X tilt for every wing root; descendants inherit it.
inline legs::Quaternion tiltWingRoot(legs::Quaternion base,float degrees,unsigned depth,float x,float y,float z){
    return depth==0?legs::bankAroundAxis(base,-degrees,x,y,z):base;
}
inline std::array<float,3> tiltedFlapAxis(const PoseSettings& p){
    const auto tilt=legs::matrix(legs::bankAroundAxis({},-p.forwardTiltDegrees,1,0,0));
    std::array<float,3> result{};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)result[i]+=tilt[i][j]*p.axis[j];
    return result;
}
inline std::array<float,3> mirroredFlapAxis(const PoseSettings& p,int side){
    auto axis=tiltedFlapAxis(p);
    // Mirroring across the body's left/right plane preserves the axial X
    // component; Y/Z change sign. Negating the entire axis reverses Twist.
    axis[1]*=static_cast<float>(side);axis[2]*=static_cast<float>(side);
    return axis;
}
// Smooth ownership and settings changes, never low-pass the ongoing wingbeat.
inline legs::Quaternion poseOutput(legs::Quaternion native,legs::Quaternion target,
    legs::Quaternion editFrom,float ownership,float editBlend){
    if(editBlend<1)target=legs::blend(editFrom,target,smooth(editBlend));
    return legs::blend(native,target,smooth(ownership));
}
}
