#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace wingflight::channel {
// Authored samples the wing model's own embedded wingbeat sequences on the
// registered bones; the other templates compute the pose procedurally.
enum class Template { legacy, awu, fairy, vampireLord, invisible, authored };
inline Template parseTemplate(std::string_view text){
    if(text=="AWU")return Template::awu;
    if(text=="Fairy")return Template::fairy;
    if(text=="VampireLord")return Template::vampireLord;
    if(text=="Invisible")return Template::invisible;
    if(text=="Authored")return Template::authored;
    return Template::legacy;
}
inline const char* templateName(Template value){
    switch(value){case Template::awu:return "AWU";case Template::fairy:return "Fairy";
    case Template::vampireLord:return "VampireLord";case Template::invisible:return "Invisible";
    case Template::authored:return "Authored";default:return "Legacy";}
}
struct PoseSettings {
    float upDegrees{50},downDegrees{-35},spreadDegrees{},forwardTiltDegrees{};
    float fastAmplitude{.25f},flutterDegrees{5},flutterPeriod{.5f};
    float jointFactor{.15f},jointDelay{.06f},nativeSwayWeight{.15f},blendSeconds{.3f};
    float flowStrength{.7f},tipDelaySeconds{.16f};
    // Articulation measured from authored wingbeats: the raised wing is folded,
    // the downstroke extends it, the lowered wing pauses, then recovery refolds.
    // Stroke bend shares the downstroke with the inner joints, so the lowered
    // wing curls down and in rather than pivoting straight from the root.
    // Zero values reproduce the earlier single-hinge stroke.
    float bottomHold{},foldDegrees{},foldCenter{.5f},featherDegrees{},strokeBendDegrees{},strokeBendCenter{.5f};
    // Per wing pair, in LeftRoots/RightRoots order. Delay follows the actual
    // driven stroke history, so parked and manual strokes remain synchronized.
    static constexpr unsigned maxPairs=8;
    std::array<float,maxPairs> pairDelaySeconds{};
    std::array<float,maxPairs> pairScale{1,1,1,1,1,1,1,1};
    std::array<float,3> axis{0,1,0}; // Character-local axis, converted to every bone's parent frame.
    // Retain authored degrees: a direction cannot recover heading at a pole,
    // or distinguish the user's -180 and +180 endpoints.
    std::optional<std::array<float,2>> axisDegrees;
};
inline std::array<float,3> axisDirection(float headingDegrees,float tiltDegrees){
    constexpr double radians=.01745329251994329577;
    const double heading=headingDegrees*radians,tilt=tiltDegrees*radians;
    const double horizontal=std::abs(tiltDegrees)==90?0:std::cos(tilt);
    return {static_cast<float>(std::sin(heading)*horizontal),static_cast<float>(std::cos(heading)*horizontal),static_cast<float>(std::sin(tilt))};
}
inline std::array<float,2> axisAngles(const std::array<float,3>& axis){
    constexpr float degrees=57.2957795130823f;
    return {std::atan2(axis[0],axis[1])*degrees,std::atan2(axis[2],std::hypot(axis[0],axis[1]))*degrees};
}
inline void setAxisAngles(PoseSettings& p,float heading,float tilt){
    p.axisDegrees=std::array{heading,tilt};p.axis=axisDirection(heading,tilt);
}
inline std::array<float,2> storedAxisAngles(const PoseSettings& p){
    return p.axisDegrees?*p.axisDegrees:axisAngles(p.axis);
}
inline PoseSettings defaults(Template kind){
    PoseSettings p;
    // Authored falls back to the AWU-style procedural stroke when a model has
    // no usable embedded sequences.
    if(kind==Template::awu||kind==Template::authored){p.bottomHold=.2f;p.foldDegrees=60;p.featherDegrees=20;p.strokeBendDegrees=35;p.strokeBendCenter=.3f;}
    if(kind==Template::fairy){p.upDegrees=25;p.downDegrees=-25;p.fastAmplitude=1;p.flutterDegrees=0;p.flutterPeriod=.12f;p.jointFactor=.1f;p.jointDelay=.01f;p.flowStrength=0;}
    if(kind==Template::vampireLord){p.upDegrees=40;p.downDegrees=-45;p.fastAmplitude=1;p.flutterDegrees=8;p.jointFactor=.2f;
        p.bottomHold=.15f;p.foldDegrees=45;p.featherDegrees=15;p.strokeBendDegrees=25;p.strokeBendCenter=.3f;}
    return p;
}
inline bool valid(const PoseSettings& p){
    const std::array values{p.upDegrees,p.downDegrees,p.spreadDegrees,p.forwardTiltDegrees,p.fastAmplitude,p.flutterDegrees,p.flutterPeriod,
        p.jointFactor,p.jointDelay,p.nativeSwayWeight,p.blendSeconds,p.flowStrength,p.tipDelaySeconds,p.axis[0],p.axis[1],p.axis[2]};
    for(float v:values)if(!std::isfinite(v))return false;
    if(!std::isfinite(p.bottomHold)||p.bottomHold<0||p.bottomHold>.45f||!std::isfinite(p.foldDegrees)||std::abs(p.foldDegrees)>180
        ||!std::isfinite(p.foldCenter)||p.foldCenter<.1f||p.foldCenter>.9f||!std::isfinite(p.featherDegrees)||std::abs(p.featherDegrees)>90
        ||!std::isfinite(p.strokeBendDegrees)||std::abs(p.strokeBendDegrees)>180
        ||!std::isfinite(p.strokeBendCenter)||p.strokeBendCenter<.1f||p.strokeBendCenter>.9f)return false;
    for(unsigned i=0;i<PoseSettings::maxPairs;++i){
        const float d=p.pairDelaySeconds[i],s=p.pairScale[i];
        if(!std::isfinite(d)||d<0||d>.5f||!std::isfinite(s)||s<0||s>2)return false;
    }
    if(p.axisDegrees){const auto& a=*p.axisDegrees;
        if(!std::isfinite(a[0])||!std::isfinite(a[1])||std::abs(a[0])>180||std::abs(a[1])>90)return false;
    }
    const float norm=p.axis[0]*p.axis[0]+p.axis[1]*p.axis[1]+p.axis[2]*p.axis[2];
    return std::abs(p.upDegrees)<=180&&std::abs(p.downDegrees)<=180&&std::abs(p.spreadDegrees)<=180&&std::abs(p.forwardTiltDegrees)<=180
        &&p.fastAmplitude>=0&&p.fastAmplitude<=2&&p.flutterDegrees>=0&&p.flutterDegrees<=60
        &&p.flutterPeriod>=.04f&&p.flutterPeriod<=10&&p.jointFactor>=0&&p.jointFactor<=1
        &&p.jointDelay>=0&&p.jointDelay<=.5f&&p.nativeSwayWeight>=0&&p.nativeSwayWeight<=1
        &&p.flowStrength>=0&&p.flowStrength<=1&&p.tipDelaySeconds>=0&&p.tipDelaySeconds<=.5f
        &&p.blendSeconds>=.02f&&p.blendSeconds<=3&&norm>.0001f&&norm<100;
}
inline float smooth(float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);}
inline float wrap(float x){return x-std::floor(x);}
// 0 = raised, 1 = lowered. After the power stroke the wing may hold low for
// part of the cycle before recovering, as authored dragon/feather wingbeats do.
inline float stroke(float phase,float downEnd,float hold=0){
    phase=wrap(phase);downEnd=std::clamp(downEnd,.1f,.7f);
    const float recover=downEnd+std::clamp(std::isfinite(hold)?hold:0.f,0.f,.9f-downEnd);
    if(phase<=downEnd)return smooth(phase/downEnd);
    if(phase<=recover)return 1;
    return 1-smooth((phase-recover)/(1-recover));
}
// 1 = folded. The raised wing is folded; the downstroke extends it (slightly
// after the root, as the elbow trails); recovery refolds it toward the top.
inline float foldCurve(float phase,float downEnd,float hold=0){
    phase=wrap(phase);downEnd=std::clamp(downEnd,.1f,.7f);
    const float recover=downEnd+std::clamp(std::isfinite(hold)?hold:0.f,0.f,.9f-downEnd);
    const float extended=std::min(downEnd*1.3f,recover);
    if(phase<=extended)return 1-smooth(phase/extended);
    if(phase<=recover)return 0;
    return smooth((phase-recover)/(1-recover));
}
// 1 = distal feathers trail. Locked open through the power stroke, snapping
// closed for recovery and open again just before the next downstroke.
inline float featherCurve(float phase,float downEnd,float hold=0){
    phase=wrap(phase);downEnd=std::clamp(downEnd,.1f,.7f);
    constexpr float reopen=.86f;
    const float closed=std::min(downEnd+std::clamp(std::isfinite(hold)?hold:0.f,0.f,.9f),reopen-.06f);
    const float closing=closed-std::max(.6f*(closed-downEnd),.08f);
    if(phase<=closing)return 0;
    if(phase<=closed)return smooth((phase-closing)/(closed-closing));
    if(phase<=reopen)return 1;
    return 1-smooth((phase-reopen)/(1-reopen));
}
// Fraction of the whole-wing fold assigned to the joint between parentSpan and
// span (0 = root, 1 = tip). Concentrated around the center; sums to one.
inline float foldShare(float center,float span,float parentSpan){
    center=std::clamp(std::isfinite(center)?center:.5f,.1f,.9f);
    auto g=[&](float s){return smooth((s-(center-.25f))/.5f);};
    const float g0=g(0),g1=g(1);if(g1-g0<.0001f)return 0;
    auto n=[&](float s){return (g(std::clamp(s,0.f,1.f))-g0)/(g1-g0);};
    return n(span)-n(parentSpan);
}
inline float featherShare(float span,float parentSpan){
    auto h=[](float s){return smooth((std::clamp(s,0.f,1.f)-.55f)/.45f);};
    return h(span)-h(parentSpan);
}
// Additional local rotation for one joint, in the stroke plane. Positive fold
// curls the wing further in the raised direction; positive stroke bend curls
// it further in the lowered direction; feathers trail opposite the stroke.
inline float articulation(const PoseSettings& p,Template kind,float powerPhase,float fastPhase,
    float flapWeight,float fastWeight,float downEnd,float span,float parentSpan){
    if(kind==Template::fairy)return 0;
    const float fold=flapWeight*foldCurve(powerPhase,downEnd,p.bottomHold)+fastWeight*.2f*foldCurve(fastPhase,downEnd);
    const float feather=flapWeight*featherCurve(powerPhase,downEnd,p.bottomHold)+fastWeight*.3f*featherCurve(fastPhase,downEnd);
    // Sprint strokes oscillate about their center, like the root's fast stroke.
    const float bend=flapWeight*stroke(powerPhase,downEnd,p.bottomHold)+fastWeight*p.fastAmplitude*(stroke(fastPhase,downEnd)-.5f);
    // Raised folding and lowered bending may happen at different joints.
    return p.foldDegrees*fold*foldShare(p.foldCenter,span,parentSpan)
        -p.strokeBendDegrees*bend*foldShare(p.strokeBendCenter,span,parentSpan)
        -p.featherDegrees*feather*featherShare(span,parentSpan);
}
inline bool articulated(const PoseSettings& p,Template kind){
    return kind!=Template::fairy&&(p.foldDegrees!=0||p.featherDegrees!=0||p.strokeBendDegrees!=0);
}
inline float angle(const PoseSettings& p,Template kind,float powerPhase,float fastPhase,float clock,
    float flapWeight,float fastWeight,float spreadWeight,float downEnd,unsigned depth,float powerRate=1,float fastRate=1){
    const float delay=p.jointDelay*static_cast<float>(depth);
    const float power=p.upDegrees+(p.downDegrees-p.upDegrees)*stroke(powerPhase-delay*powerRate/3.166667f,downEnd,p.bottomHold);
    const float center=(p.upDegrees+p.downDegrees)*.5f;
    const float fast=center+(p.upDegrees+(p.downDegrees-p.upDegrees)*stroke(fastPhase-delay*fastRate,downEnd)-center)*p.fastAmplitude;
    const float flutter=p.flutterDegrees*std::sin((clock-delay)*6.28318530718f/p.flutterPeriod);
    const float ordinary=kind==Template::fairy?
        p.upDegrees+(p.downDegrees-p.upDegrees)*(.5f-.5f*std::cos((clock-delay)*6.28318530718f/p.flutterPeriod)):
        power*flapWeight+fast*fastWeight+(p.spreadDegrees+flutter)*spreadWeight;
    return ordinary*(depth?std::pow(p.jointFactor,static_cast<float>(depth)):1.f);
}
}
