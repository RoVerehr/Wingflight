#pragma once
#include "flight.hpp"
#include "diagnostics.hpp"

namespace wingflight {
inline constexpr std::uint32_t flightStateRecord=0x57464C54; // Existing WFLT v1.
inline constexpr std::uint32_t flightModeRecord=0x57464D44;  // WFMD v1.
struct FlightSaveState {
    int resumeMode{-1};
    Mode preferredMode{Mode::free};
};

// Keep the existing airborne-resume format, plus a separate preference that
// survives landing/C exit and cannot itself cause flight to resume on load.
template<class Serialization>
bool writeFlightSave(Serialization& io, const Flight& flight, Mode preferred) {
    const std::int32_t active=flight.flying()?static_cast<std::int32_t>(flight.mode()):-1;
    const std::int32_t choice=static_cast<std::int32_t>(preferred);
    const bool stateWritten=io.WriteRecord(flightStateRecord,1,&active,sizeof(active));
    const bool preferenceWritten=io.WriteRecord(flightModeRecord,1,&choice,sizeof(choice));
    if(!stateWritten)diagnostic::report("save.state-write-failed","retain save callback failure",nullptr,"WFLT");
    if(!preferenceWritten)diagnostic::report("save.preference-write-failed","retain save callback failure",nullptr,"WFMD");
    return stateWritten && preferenceWritten;
}
template<class Serialization>
FlightSaveState readFlightSave(Serialization& io) {
    FlightSaveState result;
    std::uint32_t type{},version{},length{};
    while(io.GetNextRecordInfo(type,version,length)) {
        std::int32_t mode{};
        if(version!=1 || length!=sizeof(mode)
            || (type!=flightStateRecord && type!=flightModeRecord)){
            diagnostic::report(version!=1?"save.record-version":length!=sizeof(mode)?"save.record-length":"save.record-type",
                "skip record; keep existing defaults",nullptr,type==flightStateRecord?"WFLT":type==flightModeRecord?"WFMD":"unknown",
                static_cast<double>(version!=1?version:length!=sizeof(mode)?length:type),
                static_cast<double>(version!=1?1:length!=sizeof(mode)?sizeof(mode):flightStateRecord));continue;}
        const auto received=io.ReadRecordData(&mode,sizeof(mode));
        if(received!=sizeof(mode)){
            diagnostic::report("save.record-truncated","skip record; keep existing defaults",nullptr,
                type==flightStateRecord?"WFLT":"WFMD",received,static_cast<double>(sizeof(mode)));continue;}
        if(type==flightStateRecord && mode>=0 && mode<=3)result.resumeMode=static_cast<int>(savedMode(mode));
        if(type==flightModeRecord && (mode==0 || mode==2)) {
            result.preferredMode=savedMode(mode);
        }
        if((type==flightStateRecord&&(mode < -1||mode>3))
            ||(type==flightModeRecord&&mode!=0&&mode!=2))
            diagnostic::report("save.mode-invalid","ignore invalid mode; keep existing defaults",nullptr,
                type==flightStateRecord?"WFLT":"WFMD",mode);
    }
    // Missing preference defaults to Free Flight. Existing airborne-resume
    // records remain readable, but never infer a preference from live state.
    return result;
}
}
