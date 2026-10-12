#pragma once
#include "lower_body_policy.hpp"

namespace wingflight::legs {
struct MotionVector {float x{},y{},z{};};
struct LegMotionInput {MotionVector velocity;float gravity{4};};
struct LegRelaxation {
    bool enabled{true},secondaryMotion{true};
    float thighAnimationWeight{0.35f},maxSwingDegrees{10},stiffness{18},damping{6},inertia{1};
};
class LegRelaxationBlend {
public:
    void initialize(LegRelaxation value){current_=target_=value;}
    void setTarget(LegRelaxation value){target_=value;}
    LegRelaxation update(float dt){
        current_.enabled=target_.enabled;current_.secondaryMotion=target_.secondaryMotion;
        if(std::isfinite(dt)&&dt>0){
            const float gain=-std::expm1(-std::min(dt,0.1f)/0.25f);
            auto smooth=[&](float& current,float target){current+=(target-current)*gain;if(std::abs(current-target)<0.0001f)current=target;};
            smooth(current_.thighAnimationWeight,target_.thighAnimationWeight);
            smooth(current_.maxSwingDegrees,target_.maxSwingDegrees);smooth(current_.stiffness,target_.stiffness);
            smooth(current_.damping,target_.damping);smooth(current_.inertia,target_.inertia);
        }
        return current_;
    }
private:
    LegRelaxation current_{},target_{};
};
inline bool finite(MotionVector v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline Quaternion inverseRotation(Quaternion q){q=normalized(q);return {-q.x,-q.y,-q.z,q.w};}
inline MotionVector transform(const Matrix& m,MotionVector v){
    return {m[0][0]*v.x+m[0][1]*v.y+m[0][2]*v.z,
            m[1][0]*v.x+m[1][1]*v.y+m[1][2]*v.z,
            m[2][0]*v.x+m[2][1]*v.y+m[2][2]*v.z};
}
inline float thighFlightWeight(float flightWeight,float relaxationWeight,float authoredWeight){
    return std::clamp(flightWeight,0.0f,1.0f)*(1-std::clamp(relaxationWeight,0.0f,1.0f)*std::clamp(authoredWeight,0.0f,1.0f));
}
inline Quaternion kneeSwing(Quaternion rest,float degrees){
    const float half=degrees*0.00872664626f;
    return multiply({std::sin(half),0,0,std::cos(half)},rest);
}
// Reject takeoff/controller discontinuities, then smooth finite acceleration.
// Input is Wingflight's velocity budget in Havok units; no scene translation.
class LegAcceleration {
public:
    void reset(){*this={};}
    MotionVector update(float dt,MotionVector velocity,bool active){
        if(!active||!finite(velocity)){
            if(active)diagnostic::report("leg.velocity-nonfinite","reset secondary acceleration",this,
                !std::isfinite(velocity.x)?"x":!std::isfinite(velocity.y)?"y":"z",
                !std::isfinite(velocity.x)?velocity.x:!std::isfinite(velocity.y)?velocity.y:velocity.z);
            reset();return {};}
        if(!std::isfinite(dt)||dt<=0)return acceleration_;
        if(!seen_){previous_=velocity;seen_=true;return {};}
        dt=std::min(dt,0.1f);
        MotionVector target{(velocity.x-previous_.x)/dt,(velocity.y-previous_.y)/dt,(velocity.z-previous_.z)/dt};
        previous_=velocity;
        target.x=std::clamp(target.x,-50.0f,50.0f);target.y=std::clamp(target.y,-50.0f,50.0f);target.z=std::clamp(target.z,-50.0f,50.0f);
        const float gain=-std::expm1(-dt/0.08f);
        acceleration_.x+=(target.x-acceleration_.x)*gain;
        acceleration_.y+=(target.y-acceleration_.y)*gain;
        acceleration_.z+=(target.z-acceleration_.z)*gain;
        return acceleration_;
    }
private:
    MotionVector previous_{},acceleration_{};bool seen_{};
};
// Hinge torque about the knee parent's X axis. Normalize bone length so custom
// skeleton scales do not change the chosen angular excursion. Gravity adds a
// small hanging bias; acceleration contributes opposing inertia.
inline float kneeDrive(Quaternion rest,MotionVector footOffset,MotionVector acceleration,MotionVector gravity){
    if(!finite(footOffset)||!finite(acceleration)||!finite(gravity)){
        diagnostic::report("leg.drive-input-nonfinite","use zero knee drive",nullptr,
            !finite(footOffset)?"foot offset":!finite(acceleration)?"acceleration":"gravity");return 0;}
    const float length=std::sqrt(footOffset.x*footOffset.x+footOffset.y*footOffset.y+footOffset.z*footOffset.z);
    if(length<0.00001f||!std::isfinite(length)){
        diagnostic::report("leg.foot-lever-invalid","use zero knee drive",nullptr,{},length,0.00001);return 0;}
    const auto lever=transform(matrix(rest),{footOffset.x/length,footOffset.y/length,footOffset.z/length});
    const float g=std::max(0.1f,std::sqrt(gravity.x*gravity.x+gravity.y*gravity.y+gravity.z*gravity.z));
    const float inertiaTorque=lever.z*acceleration.y-lever.y*acceleration.z;
    const float gravityTorque=(lever.y*gravity.z-lever.z*gravity.y)/g;
    return 0.06f*inertiaTorque+0.12f*gravityTorque;
}
class DampedLegSwing {
public:
    void reset(){*this={};}
    float angle()const{return angle_;}
    float velocity()const{return velocity_;}
    float update(float dt,Quaternion parentWorld,float drive,bool active,const LegRelaxation& tuning){
        if(!tuning.secondaryMotion||!std::isfinite(tuning.maxSwingDegrees)||tuning.maxSwingDegrees<=0){reset();return 0;}
        if(!std::isfinite(dt)||dt<=0)return angle_;
        dt=std::min(dt,0.1f);parentWorld=normalized(parentWorld);
        float parentLag{};
        if(seen_&&active){
            auto delta=multiply(inverseRotation(parentWorld),previousParent_);
            if(delta.w<0)delta={-delta.x,-delta.y,-delta.z,-delta.w};
            parentLag=std::clamp(2*std::atan2(delta.x,delta.w)*57.2957795131f/dt,-360.0f,360.0f);
        }
        previousParent_=parentWorld;seen_=true;
        const float maximum=std::clamp(tuning.maxSwingDegrees,0.0f,30.0f);
        const float strength=std::isfinite(tuning.inertia)?std::clamp(tuning.inertia,0.0f,3.0f):0;
        const float target=active&&std::isfinite(drive)?maximum*std::tanh(strength*(drive+0.003f*parentLag)):0;
        const float stiffness=std::isfinite(tuning.stiffness)?std::clamp(tuning.stiffness,1.0f,100.0f):18;
        const float damping=std::isfinite(tuning.damping)?std::clamp(tuning.damping,0.0f,40.0f):6;
        // Bounded semi-implicit substeps, also stable across a capped frame hitch.
        const int steps=std::max(1,static_cast<int>(std::ceil(dt*240)));
        const float h=dt/steps;
        for(int i=0;i<steps;++i){
            velocity_+=(stiffness*(target-angle_)-damping*velocity_)*h;
            angle_+=velocity_*h;
            if(angle_>maximum){angle_=maximum;velocity_=std::min(0.0f,velocity_);}
            if(angle_<-maximum){angle_=-maximum;velocity_=std::max(0.0f,velocity_);}
        }
        if(!active&&std::abs(angle_)<0.0001f&&std::abs(velocity_)<0.0001f){angle_=velocity_=0;}
        return angle_;
    }
private:
    float angle_{},velocity_{};Quaternion previousParent_{};bool seen_{};
};
}
