#pragma once
#include <cstdint>
#include <array>

namespace wingflight::runtime {
enum class Backend { unsupported,steam1597,steam317,steam318,steam323,steam342,steam353,
    steam629,steam640,gog659,steam1130,steam1170,gog1179,steam1799,steam17104 };
struct Version {std::uint16_t major,minor,build,revision;Backend backend;};
inline constexpr std::array<Version,14> supportedVersions{{
    {1,5,97,0,Backend::steam1597},{1,6,317,0,Backend::steam317},{1,6,318,0,Backend::steam318},
    {1,6,323,0,Backend::steam323},{1,6,342,0,Backend::steam342},{1,6,353,0,Backend::steam353},
    {1,6,629,0,Backend::steam629},{1,6,640,0,Backend::steam640},{1,6,659,0,Backend::gog659},
    {1,6,1130,0,Backend::steam1130},{1,6,1170,0,Backend::steam1170},{1,6,1179,0,Backend::gog1179},
    {1,7,99,0,Backend::steam1799},{1,7,104,0,Backend::steam17104}}};
constexpr Backend select(std::uint16_t major,std::uint16_t minor,std::uint16_t build,std::uint16_t revision=0) {
    for(const auto& v:supportedVersions)if(v.major==major&&v.minor==minor&&v.build==build&&v.revision==revision)return v.backend;
    return Backend::unsupported;
}
constexpr const char* name(Backend backend) {
    switch(backend) {
    case Backend::steam1597:return "1.5.97";
    case Backend::steam317:return "1.6.317";case Backend::steam318:return "1.6.318";
    case Backend::steam323:return "1.6.323";case Backend::steam342:return "1.6.342";
    case Backend::steam353:return "1.6.353";case Backend::steam629:return "1.6.629";
    case Backend::steam640:return "1.6.640";case Backend::gog659:return "1.6.659";
    case Backend::steam1130:return "1.6.1130";case Backend::gog1179:return "1.6.1179";
    case Backend::steam1170:return "1.6.1170";
    case Backend::steam1799:return "1.7.99";
    case Backend::steam17104:return "1.7.104";
    default:return "unsupported";
    }
}
constexpr bool gog(Backend b){return b==Backend::gog659||b==Backend::gog1179;}
constexpr bool before629(Backend b){return b==Backend::steam1597||b==Backend::steam317||b==Backend::steam318
    ||b==Backend::steam323||b==Backend::steam342||b==Backend::steam353;}
constexpr bool legacy(Backend backend){return backend==Backend::steam1170;}
// Other AE builds use the resolved scene entry, not an unverified +0x94 call site.
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
