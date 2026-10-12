#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include "diagnostics.hpp"
namespace wingflight::legs {
struct Quaternion { float x{},y{},z{},w{1}; };
inline float dot(Quaternion a,Quaternion b){return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;}
inline Quaternion normalized(Quaternion q){
    const float n=std::sqrt(dot(q,q));
    if(!std::isfinite(n)||n<0.00001f){
        diagnostic::report("rotation.normalization-invalid","use identity rotation",nullptr,{},n,0.00001);return {};}
    return {q.x/n,q.y/n,q.z/n,q.w/n};
}
inline Quaternion blend(Quaternion a,Quaternion b,float weight){
    a=normalized(a);b=normalized(b);weight=std::clamp(weight,0.0f,1.0f);
    float d=dot(a,b);if(d<0){b={-b.x,-b.y,-b.z,-b.w};d=-d;}
    float x=1-weight,y=weight;
    if(d<0.9995f){const float angle=std::acos(std::clamp(d,-1.0f,1.0f));const float s=std::sin(angle);x=std::sin((1-weight)*angle)/s;y=std::sin(weight*angle)/s;}
    return normalized({x*a.x+y*b.x,x*a.y+y*b.y,x*a.z+y*b.z,x*a.w+y*b.w});
}
using Matrix=std::array<std::array<float,3>,3>;
inline Matrix matrix(Quaternion q){
    q=normalized(q);const auto [x,y,z,w]=q;
    return {{{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},
        {2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},
        {2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}}};
}
inline Quaternion multiply(Quaternion a,Quaternion b){
    return normalized({a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z});
}
inline float forwardPitch(Quaternion q){
    const auto m=matrix(q);
    // The COM's local up axis projected into the character's forward/up plane.
    // FAR forward/sprint uses negative X rotation, i.e. a positive forward pitch.
    return std::atan2(m[1][2],m[2][2])*57.2957795131f;
}
inline Quaternion forwardPitchRotation(float degrees){
    const float half=-degrees*0.00872664626f;
    return {std::sin(half),0,0,std::cos(half)};
}
inline Quaternion retainLocalBend(Quaternion source){
    const auto m=matrix(source);
    // Extract the authored local X bend from Z/Y/X orientation. Unlike the
    // projected up-axis pitch, this preserves a bend even with 90deg yaw.
    const float half=0.5f*std::atan2(m[2][1],m[2][2]);
    return {std::sin(half),0,0,std::cos(half)};
}
inline bool armedIdleAlignWanted(bool enabled,bool fullFlight,bool armed,bool fast,bool attack,bool block){
    return enabled&&fullFlight&&armed&&fast&&!attack&&!block;
}
inline Quaternion towardForwardPitch(Quaternion source,float degrees,float weight){
    // Target a pitch instead of adding it: on sheathing, FAR can already supply
    // its own ~59deg COM lean. Adding another 60deg during blend-out would double it.
    const float delta=std::remainder(degrees-forwardPitch(source),360.0f)*std::clamp(weight,0.0f,1.0f);
    const float half=-delta*0.00872664626f;
    return multiply({std::sin(half),0,0,std::cos(half)},source);
}
inline bool armedSprintLeanWanted(bool enabled,bool fullFlight,bool armed,bool highSpeed,bool excepted){
    return enabled&&fullFlight&&armed&&highSpeed&&!excepted;
}
inline Quaternion quaternion(const Matrix& m){
    Quaternion q;const float trace=m[0][0]+m[1][1]+m[2][2];
    if(trace>0){const float s=2*std::sqrt(trace+1);q={ (m[2][1]-m[1][2])/s,(m[0][2]-m[2][0])/s,(m[1][0]-m[0][1])/s,s/4};}
    else {const int i=m[0][0]>m[1][1]?(m[0][0]>m[2][2]?0:2):(m[1][1]>m[2][2]?1:2);const int j=(i+1)%3,k=(i+2)%3;
        const float s=2*std::sqrt(std::max(0.0f,1+m[i][i]-m[j][j]-m[k][k]));if(s<0.00001f)return {};
        float v[3]{};v[i]=s/4;v[j]=(m[i][j]+m[j][i])/s;v[k]=(m[i][k]+m[k][i])/s;q={v[0],v[1],v[2],(m[k][j]-m[j][k])/s};}
    return normalized(q);
}
inline float approach(float current,bool on,float dt,float seconds){
    if(!std::isfinite(dt)||dt<=0)return current;
    const float step=std::min(dt,0.1f)/std::max(seconds,0.01f);
    return on?std::min(1.0f,current+step):std::max(0.0f,current-step);
}
inline std::string canonical(std::string_view path){
    std::string out(path);for(auto& c:out){if(c=='\\')c='/';else if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');}return out;
}
// Case and slash normalization happens once when a rule/clip is received.
inline bool matches(std::string_view pattern,std::string_view path){
    std::size_t p=0,s=0,star=std::string_view::npos,retry=0;
    while(s<path.size()){
        if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==path[s])){++p;++s;}
        else if(p<pattern.size()&&pattern[p]=='*'){star=p++;retry=s;}
        else if(star!=std::string_view::npos){p=star+1;s=++retry;}
        else return false;
    }
    while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();
}
}
