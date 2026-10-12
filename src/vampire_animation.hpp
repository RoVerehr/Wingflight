#pragma once
#include "lower_body_policy.hpp"
#include <span>
#include <vector>
#include <map>
#include <cstring>
#include <stdexcept>
#include <cstdint>

namespace wingflight::vampire {
// A bounded, read-only reader for the original Skyrim 64-bit HKX packfile.
// Never instantiate serialized Havok objects or execute anything from the file.
class Packfile {
public:
    struct Section {std::uint32_t base,local,global,virtuals,exports,imports,end;};
    explicit Packfile(std::span<const std::uint8_t> b):bytes(b){
        require(b.size()>=208&&b.size()<=8*1024*1024,"packfile size");
        require(u(0)==0x57e0e057&&u(4)==0x10c0c010,"packfile signature");
        require(u(12)==8&&std::memcmp(b.data()+40,"hk_2010.2.0-r1",13)==0,"packfile Havok version");
        require(b[16]==8&&b[17]==1&&u(20)==3,"packfile architecture/sections");
        for(unsigned i=0;i<3;++i){Section s{};std::memcpy(&s,b.data()+84+48*i,sizeof(s));
            require(s.local<=s.global&&s.global<=s.virtuals&&s.virtuals<=s.exports&&s.exports<=s.imports&&s.imports<=s.end,"section order");
            require(range(s.base,s.end),"section bounds");sections.push_back(s);}
        for(const auto& s:sections){
            for(std::size_t p=s.base+s.local;p+8<=s.base+s.global;p+=8){
                auto from=u(p),to=u(p+4);if(from==0xffffffff)continue;
                require(from<=s.local&&s.local-from>=8&&to<s.local,"local fixup");
                require(pointers.emplace(s.base+from,s.base+to).second,"duplicate pointer");}
            for(std::size_t p=s.base+s.global;p+12<=s.base+s.virtuals;p+=12){
                auto from=u(p),index=u(p+4),to=u(p+8);if(from==0xffffffff)continue;
                require(index<3&&from<=s.local&&s.local-from>=8&&to<sections[index].local,"global fixup");
                require(pointers.emplace(s.base+from,sections[index].base+to).second,"duplicate pointer");}
            for(std::size_t p=s.base+s.virtuals;p+12<=s.base+s.exports;p+=12){
                auto from=u(p),index=u(p+4),to=u(p+8);if(from==0xffffffff)continue;
                require(index<3&&from<s.local&&to<sections[index].local,"class fixup");
                classes.emplace(s.base+from,text(sections[index].base+to));}
            require(s.exports==s.imports&&s.imports==s.end,"external imports unsupported");
        }
    }
    static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
    bool range(std::size_t p,std::size_t n)const{return p<=bytes.size()&&n<=bytes.size()-p;}
    template<class T>T get(std::size_t p)const{require(range(p,sizeof(T)),"read bounds");T v;std::memcpy(&v,bytes.data()+p,sizeof(T));return v;}
    std::uint32_t u(std::size_t p)const{return get<std::uint32_t>(p);}
    std::size_t ptr(std::size_t p)const{auto i=pointers.find(p);require(i!=pointers.end(),"missing pointer fixup");return i->second;}
    std::string text(std::size_t p)const{require(range(p,1),"string bounds");std::size_t n=0;
        while(range(p+n,1)&&bytes[p+n]&&n<2048)++n;
        require(n<2048&&range(p+n,1),"unterminated string");return {reinterpret_cast<const char*>(bytes.data()+p),n};}
    std::size_t object(std::string_view name)const{std::size_t found=0;for(auto& [p,c]:classes)if(c==name){require(!found,"duplicate class object");found=p;}
        require(found!=0,"class object absent");return found;}
    std::vector<std::size_t> objects(std::string_view name)const{std::vector<std::size_t> out;for(auto& [p,c]:classes)if(c==name)out.push_back(p);return out;}
    std::span<const std::uint8_t> array(std::size_t p,unsigned stride,unsigned limit)const{
        auto count=u(p+8);require(count<=limit,"array count");if(!count)return {};
        const auto target=ptr(p);require(range(target,std::size_t(count)*stride),"array bounds");return bytes.subspan(target,std::size_t(count)*stride);}
private:
    std::span<const std::uint8_t> bytes;
    std::vector<Section> sections;
    std::map<std::size_t,std::size_t> pointers;
    std::map<std::size_t,std::string> classes;
};

struct Curve {
    unsigned degree{};
    std::vector<std::uint8_t> knots;
    std::vector<legs::Quaternion> control;
    legs::Quaternion at(float f)const{
        if(knots.empty())return control.front();
        f=std::clamp(f,float(knots[degree]),float(knots[control.size()]));
        std::size_t span=control.size()-1;
        if(f<knots[control.size()]){
            const auto it=std::upper_bound(knots.begin()+degree,knots.begin()+control.size()+1,f);
            span=std::size_t(it-knots.begin()-1);}
        // Standard de Boor evaluation in quaternion component space. Normalize
        // afterwards, matching the spline format's unit orientation semantics.
        std::array<legs::Quaternion,5> d{};
        for(unsigned j=0;j<=degree;++j)d[j]=control[span-degree+j];
        for(unsigned r=1;r<=degree;++r)for(unsigned j=degree;j>=r;--j){
            const auto i=span-degree+j;float den=float(knots[i+degree-r+1])-knots[i];
            const float a=den>0?(f-knots[i])/den:0;
            const auto x=d[j-1],y=d[j];d[j]={x.x+(y.x-x.x)*a,x.y+(y.y-x.y)*a,x.z+(y.z-x.z)*a,x.w+(y.w-x.w)*a};}
        return legs::normalized(d[degree]);
    }
};
class Stream {
public:
    explicit Stream(std::span<const std::uint8_t> b):bytes(b){}
    template<class T>T get(){Packfile::require(pos<=bytes.size()&&sizeof(T)<=bytes.size()-pos,"spline read bounds");T v;std::memcpy(&v,bytes.data()+pos,sizeof(v));pos+=sizeof(v);return v;}
    void skip(std::size_t n){Packfile::require(pos<=bytes.size()&&n<=bytes.size()-pos,"spline skip bounds");pos+=n;}
    void align(unsigned n){skip((n-pos%n)%n);}
    unsigned header(Curve& c){const auto count=get<std::uint16_t>()+1u;c.degree=get<std::uint8_t>();
        Packfile::require(count<=256&&count>c.degree&&c.degree<=4,"spline degree/control count");
        c.knots.resize(count+c.degree+1);for(auto& k:c.knots)k=get<std::uint8_t>();
        Packfile::require(std::is_sorted(c.knots.begin(),c.knots.end())&&c.knots[c.degree]<c.knots[count],"spline knots");return count;}
    void vector(unsigned flags,unsigned quant){
        Packfile::require(quant<=1&&!(flags&0x88),"vector encoding");unsigned count=0,axes=0;
        if(flags&0x70){Curve c;count=header(c);align(4);}
        for(unsigned axis=0;axis<3;++axis){if(flags&(0x10u<<axis)){skip(8);++axes;}else if(flags&(1u<<axis))skip(4);}
        skip(std::size_t(count)*axes*(quant+1));
    }
    legs::Quaternion quaternion(unsigned quant){
        if(quant==1){std::uint64_t v=0;for(unsigned i=0;i<5;++i)v|=std::uint64_t(get<std::uint8_t>())<<(8*i);
            const unsigned omitted=unsigned(v>>36)&3;float components[4]{};float square=0;unsigned j=0;
            for(unsigned i=0;i<4;++i)if(i!=omitted){const float f=(float((v>>(12*j++))&4095)-2047.0f)*(1.4142135623730951f/4094.0f);components[i]=f;square+=f*f;}
            Packfile::require(square<1.01f,"compressed quaternion length");
            components[omitted]=std::sqrt(std::max(0.0f,1-square))*((v&(1ULL<<38))?-1.0f:1.0f);
            return {components[0],components[1],components[2],components[3]};}
        if(quant==5){legs::Quaternion q{get<float>(),get<float>(),get<float>(),get<float>()};
            Packfile::require(std::isfinite(legs::dot(q,q))&&legs::dot(q,q)>0.5f,"quaternion length");return legs::normalized(q);}
        throw std::runtime_error("rotation quantization unsupported (expected vanilla 40-bit)");
    }
    Curve rotation(unsigned flags,unsigned quant){
        Packfile::require(quant==1||quant==5,"rotation quantization");Curve c;
        if(flags&0xf0){const auto count=header(c);align(quant==5?4:1);c.control.reserve(count);for(unsigned i=0;i<count;++i)c.control.push_back(quaternion(quant));}
        else c.control.push_back(flags&15?quaternion(quant):legs::Quaternion{});
        return c;
    }
private:std::span<const std::uint8_t> bytes;std::size_t pos{};
};
struct Skeleton {
    std::vector<std::string> names;
    static Skeleton read(std::span<const std::uint8_t> bytes){Packfile p(bytes);Skeleton chosen;
        // skeleton.hkx contains both animation and reduced ragdoll skeletons.
        // Select the full wing-bearing skeleton by names, never object order.
        for(const auto s:p.objects("hkaSkeleton")){
            const auto a=p.array(s+0x28,16,1024);Skeleton out;const auto target=p.ptr(s+0x28);
            for(std::size_t i=0;i<a.size()/16;++i)out.names.push_back(p.text(p.ptr(target+16*i)));
            if(std::find(out.names.begin(),out.names.end(),"LeftWing5")!=out.names.end()
                &&std::find(out.names.begin(),out.names.end(),"RightWing5")!=out.names.end()){
                Packfile::require(chosen.names.empty(),"ambiguous wing skeleton");chosen=std::move(out);}}
        Packfile::require(chosen.names.size()>=10,"wing skeleton absent");return chosen;}
};
struct Animation {
    float duration{};unsigned frames{};
    std::vector<Curve> rotations;
    std::vector<std::uint16_t> boneIndices;
    static Animation read(std::span<const std::uint8_t> bytes,const Skeleton& skeleton){
        Packfile p(bytes);const auto a=p.object("hkaSplineCompressedAnimation"),b=p.object("hkaAnimationBinding");
        Animation out;out.duration=p.get<float>(a+0x14);out.frames=p.u(a+0x38);const auto tracks=p.u(a+0x18);
        Packfile::require(std::isfinite(out.duration)&&out.duration>0&&out.duration<60&&out.frames>=2&&out.frames<=256,"animation time/frame count");
        Packfile::require(p.u(a+0x1c)==0&&p.u(a+0x3c)==1&&tracks>0&&tracks<=1024&&p.u(a+0xa8)==0,"animation format (single block)");
        Packfile::require(p.ptr(b+0x18)==a&&p.get<std::uint8_t>(b+0x40)==0,"binding normal blend");
        const auto indices=p.array(b+0x20,2,1024);out.boneIndices.resize(tracks);
        if(indices.empty()){
            Packfile::require(tracks==skeleton.names.size(),"identity binding track count");
            for(unsigned i=0;i<tracks;++i)out.boneIndices[i]=static_cast<std::uint16_t>(i);
        }else{
            Packfile::require(indices.size()/2==tracks,"binding track count");
            for(unsigned i=0;i<tracks;++i){std::int16_t id;std::memcpy(&id,indices.data()+i*2,2);Packfile::require(id>=0&&std::size_t(id)<skeleton.names.size(),"binding bone index");out.boneIndices[i]=static_cast<std::uint16_t>(id);}}
        Stream data(p.array(a+0x98,1,8*1024*1024));std::vector<std::array<unsigned,4>> masks;
        for(unsigned i=0;i<tracks;++i){const auto q=data.get<std::uint8_t>();masks.push_back({q,data.get<std::uint8_t>(),data.get<std::uint8_t>(),data.get<std::uint8_t>()});}
        data.align(4);out.rotations.reserve(tracks);
        for(const auto& h:masks){data.vector(h[1],h[0]&3);data.align(4);out.rotations.push_back(data.rotation(h[2],(h[0]>>2)&15));data.align(4);data.vector(h[3],h[0]>>6);data.align(4);}
        return out;
    }
    legs::Quaternion sample(std::size_t track,float phase)const{
        return rotations.at(track).at(std::clamp(phase,0.0f,1.0f)*float(frames-1));}
};
inline constexpr std::array<const char*,10> wingNames{"LeftWing1","LeftWing2","LeftWing3","LeftWing4","LeftWing5","RightWing1","RightWing2","RightWing3","RightWing4","RightWing5"};
inline float sourcePhase(float phase,float peak,float bottom,float downEnd){
    const float arc=std::fmod(bottom-peak+1,1.0f);
    const float distance=phase<=downEnd?arc*phase/downEnd:arc+(1-arc)*(phase-downEnd)/(1-downEnd);
    return std::fmod(peak+distance,1.0f);
}
}
