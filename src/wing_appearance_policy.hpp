#pragma once
#include <cmath>
#include <array>
#include <optional>
#include <algorithm>
#include <string>
#include <string_view>
#include <cstdint>

namespace wingflight::appearance {
struct Settings {
    float scalePercent{100};
    float verticalOffset{};
    bool neutral() const { return scalePercent==100 && verticalOffset==0; }
};
struct ProfileSettings {
    float normalScale{100},flightScale{100},verticalOffset{};
};
inline bool valid(ProfileSettings p){
    return std::isfinite(p.normalScale)&&p.normalScale>=10&&p.normalScale<=200
        &&std::isfinite(p.flightScale)&&p.flightScale>=10&&p.flightScale<=200
        &&std::isfinite(p.verticalOffset)&&p.verticalOffset>=-60&&p.verticalOffset<=60;
}
inline ProfileSettings legacy(Settings p){return {p.scalePercent,p.scalePercent,p.verticalOffset};}
inline ProfileSettings rounded(ProfileSettings p){
    p.normalScale=std::round(p.normalScale*100)/100;p.flightScale=std::round(p.flightScale*100)/100;
    p.verticalOffset=std::round(p.verticalOffset*100)/100;return p;
}
// Encode the filename, rather than excluding punctuation or translated names.
inline std::string formKey(std::string_view plugin,std::string_view kind,std::uint32_t localID){
    constexpr char digits[]="0123456789abcdef";std::string out="Wing_";
    for(unsigned char c:plugin){if(c>='A'&&c<='Z')c+=32;out+=digits[c>>4];out+=digits[c&15];}
    out+='_';out+=kind;out+='_';
    for(int shift=28;shift>=0;shift-=4)out+=digits[(localID>>shift)&15];
    for(auto& c:out)if(c>='A'&&c<='Z')c+=32;return out;
}
struct FlightBlend {
    bool initialized{},target{};
    float weight{},from{},elapsed{};
    void update(bool flying,float delta){
        if(!initialized){initialized=true;target=flying;weight=from=flying?1.f:0.f;elapsed=.5f;return;}
        if(target!=flying){target=flying;from=weight;elapsed=0;}
        if(std::isfinite(delta)&&delta>0)elapsed=std::min(.5f,elapsed+delta);
        const float t=elapsed*2,s=t*t*(3-2*t);weight=from+((target?1.f:0.f)-from)*s;
    }
    float scale(ProfileSettings p)const{return p.normalScale+(p.flightScale-p.normalScale)*weight;}
};
// Losing the spell/provider is not the end of its visible StopEffect animation.
// Keep only a previously bound ART still owned by the engine's effect list.
// finished is deliberately not an input: engine removal/attachment is decisive.
inline bool retainRemovedArt(bool listed,bool attached,bool sameModel){return listed&&attached&&sameModel;}
// An independent channel owns only its last write. Native animation or another
// mod may replace one channel without replacing the other three.
struct Channel {
    float source{},output{};
    bool wrote{};
    float apply(float current,float factor,float offset) {
        if(!wrote||current!=output)source=current;
        output=source*factor+offset;wrote=true;return output;
    }
    float restore(float current) {
        const float result=wrote&&current==output?source:current;
        wrote=false;return result;
    }
};
// Convert torso up into the wing root parent's axes. The actor scale is
// intentional; changing wing size must not change the attachment displacement.
template<class Matrix>std::optional<std::array<float,3>> localOffset(float height,float actorScale,float parentScale,const Matrix& torso,const Matrix& parent){
    if(!std::isfinite(height)||!std::isfinite(actorScale)||!std::isfinite(parentScale)||std::abs(parentScale)<.00001f)return {};
    std::array<float,3> result{};
    for(unsigned column=0;column<3;++column){
        for(unsigned row=0;row<3;++row)result[column]+=parent[row][column]*torso[row][2];
        result[column]*=height*actorScale/parentScale;
        if(!std::isfinite(result[column]))return {};
    }
    return result;
}
}
