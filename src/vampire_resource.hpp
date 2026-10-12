#pragma once
// Read original motion from the user's game archive. No extracted game assets
// are included in Wingflight. Replaced humanoid body clips remain untouched.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace wingflight::vampire {
inline void archiveRequire(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline std::string assetPath(std::string s){for(auto& c:s){if(c=='/')c='\\';if(c>='A'&&c<='Z')c+=32;}return s;}
inline std::vector<std::uint8_t> lz4Block(std::span<const std::uint8_t> src,std::size_t limit){
    std::vector<std::uint8_t> out;std::size_t p=0;
    auto extended=[&](std::size_t n){if(n==15){unsigned b;do{archiveRequire(p<src.size(),"LZ4 length truncated");b=src[p++];n+=b;archiveRequire(n<=limit,"LZ4 length excessive");}while(b==255);}return n;};
    while(p<src.size()){
        unsigned token=src[p++];auto n=extended(token>>4);
        archiveRequire(n<=src.size()-p&&n<=limit-out.size(),"LZ4 literal bounds");
        out.insert(out.end(),src.begin()+p,src.begin()+p+n);p+=n;if(p==src.size())break;
        archiveRequire(src.size()-p>=2,"LZ4 offset truncated");std::size_t offset=src[p]|(src[p+1]<<8);p+=2;
        n=extended(token&15)+4;archiveRequire(offset&&offset<=out.size()&&n<=limit-out.size(),"LZ4 match bounds");
        for(std::size_t i=0;i<n;++i)out.push_back(out[out.size()-offset]);
    }
    return out;
}
inline std::vector<std::uint8_t> lz4Asset(std::span<const std::uint8_t> src,std::size_t size){
    archiveRequire(size>0&&size<=8*1024*1024,"BSA expanded size");
    if(src.size()<4||src[0]!=4||src[1]!=0x22||src[2]!=0x4d||src[3]!=0x18){
        auto out=lz4Block(src,size);archiveRequire(out.size()==size,"LZ4 expanded size mismatch");return out;}
    archiveRequire(src.size()>=7,"LZ4 frame header");unsigned flags=src[4];
    archiveRequire((flags&0xc0)==0x40&&(flags&0x20),"LZ4 frame version/dependent blocks");
    std::size_t p=7+((flags&8)?8:0)+((flags&1)?4:0);archiveRequire(p<=src.size(),"LZ4 optional header");
    auto word=[&](){archiveRequire(src.size()-p>=4,"LZ4 frame word");std::uint32_t v;std::memcpy(&v,src.data()+p,4);p+=4;return v;};
    std::vector<std::uint8_t> out;
    for(;;){auto block=word();if(!block)break;auto n=block&0x7fffffff;
        archiveRequire(n<=src.size()-p,"LZ4 frame block bounds");auto b=src.subspan(p,n);p+=n;
        auto part=(block&0x80000000)?std::vector<std::uint8_t>(b.begin(),b.end()):lz4Block(b,size-out.size());
        archiveRequire(part.size()<=size-out.size(),"LZ4 frame expanded bounds");out.insert(out.end(),part.begin(),part.end());
        if(flags&16)(void)word();}
    if(flags&4)(void)word();archiveRequire(out.size()==size&&p==src.size(),"LZ4 frame size/trailing data");return out;
}
inline std::vector<std::vector<std::uint8_t>> originalAssets(const std::filesystem::path& path,const std::vector<std::string>& requested){
    std::ifstream f(path,std::ios::binary|std::ios::ate);archiveRequire(bool(f),"original Skyrim animation archive unavailable");
    const auto fileSize=static_cast<std::uint64_t>(f.tellg());std::uint64_t pos=0;
    auto seek=[&](std::uint64_t p){archiveRequire(p<=fileSize,"BSA seek bounds");f.clear();f.seekg(static_cast<std::streamoff>(p));pos=p;};
    auto read=[&](std::size_t n){archiveRequire(n<=fileSize-pos&&n<=32*1024*1024,"BSA read bounds");std::vector<std::uint8_t> b(n);archiveRequire(bool(f.read(reinterpret_cast<char*>(b.data()),n)),"BSA short read");pos+=n;return b;};
    seek(0);auto h=read(36);auto word=[&](unsigned offset){std::uint32_t v;std::memcpy(&v,h.data()+offset,4);return v;};
    archiveRequire(word(0)==0x00415342&&word(4)==105,"BSA signature/version");
    auto flags=word(12),folderCount=word(16),fileCount=word(20),nameBytes=word(28);
    archiveRequire((flags&3)==3&&folderCount<=8192&&fileCount<=200000&&nameBytes<=32*1024*1024,"BSA index format/limits");
    seek(word(8));auto folders=read(folderCount*24);
    struct Entry{std::string folder;std::uint32_t size,offset;};std::vector<Entry> entries;
    for(unsigned i=0;i<folderCount;++i){std::uint32_t count;std::memcpy(&count,folders.data()+i*24+8,4);
        archiveRequire(count<=fileCount-entries.size(),"BSA folder file count");auto len=read(1)[0];archiveRequire(len>0,"BSA folder name empty");
        auto name=read(len);archiveRequire(name.back()==0,"BSA folder name terminator");std::string folder(name.begin(),name.end()-1);
        auto records=read(count*16);for(unsigned j=0;j<count;++j){Entry e{assetPath(folder),0,0};std::memcpy(&e.size,records.data()+j*16+8,4);std::memcpy(&e.offset,records.data()+j*16+12,4);entries.push_back(std::move(e));}}
    archiveRequire(entries.size()==fileCount,"BSA index file count");auto names=read(nameBytes);std::size_t p=0;
    std::vector<const Entry*> matches(requested.size());
    for(const auto& e:entries){auto first=p;while(p<names.size()&&names[p])++p;archiveRequire(p<names.size(),"BSA file name terminator");
        auto full=e.folder+"\\"+assetPath(std::string(names.begin()+first,names.begin()+p));++p;
        for(std::size_t k=0;k<requested.size();++k)if(full==assetPath(requested[k])){archiveRequire(!matches[k],"BSA duplicate requested asset");matches[k]=&e;}}
    archiveRequire(p==names.size(),"BSA file name pool mismatch");std::vector<std::vector<std::uint8_t>> result;
    for(const auto* e:matches){archiveRequire(e,"original wing animation absent in archive");auto size=e->size&0x3fffffff;
        archiveRequire(size<=8*1024*1024,"BSA asset compressed size");seek(e->offset);auto blob=read(size);std::span<const std::uint8_t> payload(blob);
        if(flags&0x100){archiveRequire(!payload.empty()&&payload[0]<payload.size(),"BSA embedded name");payload=payload.subspan(1+payload[0]);}
        if(bool(flags&4)!=bool(e->size&0x40000000)){archiveRequire(payload.size()>=4,"BSA expanded size missing");std::uint32_t expanded;std::memcpy(&expanded,payload.data(),4);result.push_back(lz4Asset(payload.subspan(4),expanded));}
        else result.emplace_back(payload.begin(),payload.end());}
    return result;
}
}
