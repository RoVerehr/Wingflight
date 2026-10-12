#pragma once
#include <cstdint>

namespace wingflight::runtime {
enum class Backend { unsupported, steam1597, steam1170, steam1799, steam17104 };
constexpr Backend select(std::uint16_t major,std::uint16_t minor,std::uint16_t build,std::uint16_t revision=0) {
    if(major!=1||revision!=0)return Backend::unsupported;
    if(minor==5&&build==97)return Backend::steam1597;
    if(minor==6&&build==1170)return Backend::steam1170;
    if(minor==7&&build==99)return Backend::steam1799;
    if(minor==7&&build==104)return Backend::steam17104;
    return Backend::unsupported;
}
constexpr const char* name(Backend backend) {
    switch(backend) {
    case Backend::steam1597:return "1.5.97";
    case Backend::steam1170:return "1.6.1170";
    case Backend::steam1799:return "1.7.99";
    case Backend::steam17104:return "1.7.104";
    default:return "unsupported";
    }
}
constexpr bool legacy(Backend backend){return backend==Backend::steam1170;}
constexpr bool bodyCallsite(Backend backend){return backend==Backend::steam1597||legacy(backend);}
constexpr std::uint64_t wingEntryId(Backend backend,std::uint64_t aeId){
    if(backend!=Backend::steam1597)return aeId;
    switch(aeId){
    case 72576:return 70982;
    case 62932:return 61997;
    case 70191:return 68839;
    case 72783:return 71185;
    default:return 0;
    }
}
constexpr bool supported(Backend backend){return backend!=Backend::unsupported;}
constexpr bool newAddressFormat(Backend backend){return backend==Backend::steam1799||backend==Backend::steam17104;}
constexpr bool matchingEntrypoint(std::uintptr_t actual,std::uintptr_t expected,bool executable) {
    return actual!=0&&actual==expected&&executable;
}
}
