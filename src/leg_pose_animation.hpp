#pragma once
#include "vampire_animation.hpp"
#include "flight_transition_policy.hpp"
namespace wingflight::legs {
inline constexpr std::array<const char*,8> poseBoneNames{
    "NPC L Thigh [LThg]","NPC L Calf [LClf]","NPC L Foot [Lft ]","NPC L Toe0 [LToe]",
    "NPC R Thigh [RThg]","NPC R Calf [RClf]","NPC R Foot [Rft ]","NPC R Toe0 [RToe]"};
using LegPose=std::array<Quaternion,8>;
struct LegPoseAnimation {
    float duration{};unsigned frames{};
    std::array<vampire::Curve,8> rotations;
    static LegPoseAnimation read(std::span<const std::uint8_t> bytes){
        vampire::Packfile file(bytes);const auto object=file.object("hkaSplineCompressedAnimation");
        const auto count=file.u(object+0x18);
        const auto annotationBytes=file.array(object+0x28,24,1024);
        vampire::Packfile::require(annotationBytes.size()/24==count,"leg pose needs named animation tracks");
        vampire::Skeleton trackLayout;trackLayout.names.resize(count);
        const auto annotations=file.ptr(object+0x28);
        for(unsigned i=0;i<count;++i)trackLayout.names[i]=file.text(file.ptr(annotations+24*i));
        const auto animation=vampire::Animation::read(bytes,trackLayout);
        LegPoseAnimation result;result.duration=animation.duration;result.frames=animation.frames;
        for(unsigned i=0;i<poseBoneNames.size();++i){
            auto found=std::find(trackLayout.names.begin(),trackLayout.names.end(),poseBoneNames[i]);
            vampire::Packfile::require(found!=trackLayout.names.end(),"leg pose bone track absent");
            const auto track=std::size_t(found-trackLayout.names.begin());
            vampire::Packfile::require(std::find(found+1,trackLayout.names.end(),poseBoneNames[i])==trackLayout.names.end(),"duplicate leg pose bone track");
            result.rotations[i]=animation.rotations.at(track);
        }
        return result;
    }
    LegPose sample(float seconds)const{
        LegPose result;
        const float phase=std::fmod(std::max(0.0f,seconds),duration)/duration;
        for(unsigned i=0;i<result.size();++i)result[i]=rotations[i].at(phase*float(frames-1));
        return result;
    }
};
// Blend the three live reference loops, not three frozen poses. Restarting a
// transition from the displayed weights keeps rapid reversals continuous.
class LegPoseBlend {
public:
    void reset(){*this={};}
    std::array<float,3> update(unsigned mode,float dt,const pose::TransitionTiming& timing){
        mode=std::min(mode,2u);
        if(!initialized_){weights_={};weights_[mode]=1;mode_=mode;initialized_=true;return weights_;}
        if(mode!=mode_){
            from_=weights_;seconds_=mode==2?timing.sprintIn:mode_==2?timing.sprintOut:timing.movement;
            mode_=mode;age_=0;
        }
        if(!std::isfinite(dt)||dt<=0)return weights_;
        age_=std::min(seconds_,age_+std::min(dt,0.1f));
        const float x=seconds_>0?age_/seconds_:1,eased=x*x*(3-2*x);
        for(unsigned i=0;i<3;++i)weights_[i]=from_[i]*(1-eased)+(i==mode_?eased:0);
        return weights_;
    }
private:
    bool initialized_{};unsigned mode_{};float age_{},seconds_{};
    std::array<float,3> weights_{1,0,0},from_{1,0,0};
};
inline unsigned legPoseMode(bool fast,float x,float y){return fast?2:std::hypot(x,y)>0.1f?1:0;}
inline LegPose mixLegPoses(const std::array<LegPose,3>& poses,const std::array<float,3>& weights){
    LegPose out=poses[0];float sum=weights[0];
    for(unsigned mode=1;mode<3;++mode){
        if(weights[mode]<=0)continue;
        const float next=sum+weights[mode];
        for(unsigned i=0;i<out.size();++i)out[i]=blend(out[i],poses[mode][i],weights[mode]/next);
        sum=next;
    }
    return out;
}
}
