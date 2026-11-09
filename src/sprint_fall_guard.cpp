#include "sprint_fall_guard.hpp"
#include "native_contract.hpp"
#include "runtime_support.hpp"
#include "runtime_policy.hpp"
#include "runtime_callable.hpp"
#include "runtime_diagnostic.hpp"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <cstring>
#include <optional>
#include <unordered_set>

namespace wingflight {
namespace {
using Function=runtime::FunctionBounds;
std::optional<Function> nativeFunction(std::uintptr_t address,std::uintptr_t base=REL::Module::get().base()){
    const auto lookup=[base](std::uintptr_t pc)->std::optional<runtime::UnwindFunction>{
        DWORD64 image{};const auto entry=RtlLookupFunctionEntry(pc,&image,nullptr);
        if(!entry||image!=base)return {};
        return runtime::UnwindFunction{entry->BeginAddress,entry->EndAddress,entry->UnwindData};
    };
    const auto info=[base](std::uint32_t rva)->std::span<const std::uint8_t>{
        const auto data=reinterpret_cast<const std::uint8_t*>(base+rva);
        if(!runtimeReadable(data,4))return {};
        const auto size=4+((static_cast<unsigned>(data[2])+1)&~1U)*2+((data[0]&0x20)?12:0);
        if(!runtimeReadable(data,size))return {};
        return {data,size};
    };
    const auto f=runtime::nativeFunctionBounds(address,base,lookup,info);
    if(!f||!runtimeReadable(reinterpret_cast<void*>(f->begin),f->end-f->begin))return {};
    return f;
}
std::span<const std::uint8_t> bytes(Function f){
    return {reinterpret_cast<const std::uint8_t*>(f.begin),f.end-f.begin};
}
SprintFallEligibility eligible{};
using Check=bool(*)(RE::Actor*);
Check original{};
std::array<std::uintptr_t,3> callers{};
std::array<bool,3> overriding{},observed{};
const runtime::PointerReader pointer=[](std::uintptr_t slot)->std::optional<std::uintptr_t>{
    if(!runtimeReadable(reinterpret_cast<void*>(slot),sizeof(std::uintptr_t)))return {};
    std::uintptr_t value{};std::memcpy(&value,reinterpret_cast<void*>(slot),sizeof(value));return value;
};
std::uintptr_t transferTarget(std::uintptr_t target){
    for(unsigned depth=0;depth<8;++depth){
        if(!runtime::executableMemory(target)||!runtimeReadable(reinterpret_cast<void*>(target),16))return 0;
        const auto transfer=runtime::inspectEntryTransfer(target,{reinterpret_cast<const std::uint8_t*>(target),16});
        if(transfer.kind==runtime::EntryTransferKind::none)return target;
        const auto next=transfer.kind==runtime::EntryTransferKind::indirectJumpSlot?pointer(transfer.address).value_or(0):transfer.address;
        if(!next||next==target)return 0;
        target=next;
    }
    return 0;
}
std::uintptr_t replacementCaller(std::uintptr_t fall){
    const auto update=nativeFunction(REL::RelocationID(39375,40447).address());
    if(!update)return 0;
    const auto changed=REL::RelocationID(41271,42350).address();
    std::unordered_set<std::uintptr_t> inspected;
    std::uintptr_t caller{};unsigned matches{};
    for(const auto& call:runtime::directCalls(update->begin,bytes(*update))){
        const auto target=transferTarget(call.target);
        if(!target)continue;
        HMODULE owner{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target),&owner)||reinterpret_cast<std::uintptr_t>(owner)==REL::Module::get().base())continue;
        const auto body=nativeFunction(target,reinterpret_cast<std::uintptr_t>(owner));
        if(!body||!inspected.insert(body->begin).second)continue;
        const auto found=runtime::replacementSprintFallCall(body->begin,bytes(*body),fall,changed,pointer);
        if(!found.size)continue;
        caller=body->begin+found.offset+found.size;++matches;
        SKSE::log::info("Flight sprint replacement found: nativeDispatch={:X} target={:X} fallingReturn={:X}; predicate, stamina, cancellation and native sprint-change callback verified",update->begin+call.offset,target,caller);
    }
    if(matches!=1){
        if(matches)SKSE::log::warn("Flight sprint replacement ignored: semantic matches={}; no broad caller exception",matches);
        return 0;
    }
    return caller;
}
__declspec(noinline) bool check(RE::Actor* actor){
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool native=original(actor); // Retain the current predicate/call chain.
    std::size_t branch{};while(branch<callers.size()&&(!callers[branch]||callers[branch]!=caller))++branch;
    if(branch==callers.size())return native;
    const bool owned=eligible&&eligible(actor);
    const bool result=sprintFallingResult(native,owned,caller,callers);
    const bool override=native&&!result;
    if(owned&&(!observed[branch]||override!=overriding[branch])){
        observed[branch]=true;overriding[branch]=override;
        SKSE::log::debug("Flight sprint falling check: branch={} nativeFalling={} bypass={} caller={:X} actor={:X}; native action, pose and stamina remain authoritative",
            branch==0?"qualification":branch==1?"SprintStart":"dispatched replacement",native,override,caller,reinterpret_cast<std::uintptr_t>(actor));
    }
    return result;
}
}
void installSprintFallGuard(SprintFallEligibility predicate){
    if(original)return;
    const auto backend=runtimeBackend();
    const auto midair=REL::RelocationID(36259,37243).address();
    const auto overencumbered=REL::RelocationID(36457,37453).address();
    const auto processOffset=runtime::before629(backend)?0xF0U:0xF8U;
    const auto requestOffset=runtime::before629(backend)?0xBDDU:
        runtime::newAddressFormat(backend)?0xBEDU:0xBE5U;
    std::unordered_set<std::uintptr_t> seen,falling;
    // IsFalling is adjacent to the already-resolved native IsInMidair entry.
    // Inspect actual function boundaries/semantics, never assume an AE offset.
    for(std::uintptr_t address=midair;address<midair+0x400;address+=0x10){
        const auto f=nativeFunction(address);
        if(f&&seen.insert(f->begin).second&&runtime::fallingPredicate(bytes(*f),processOffset))falling.insert(f->begin);
    }
    if(falling.size()!=1){
        SKSE::log::warn("Flight sprint falling guard unavailable: native falling predicate matches={} runtime={}; existing sprint behavior retained",falling.size(),runtimeName());return;
    }
    const auto fall=*falling.begin();
    const auto text=REL::Module::get().segment(REL::Segment::textx);
    if(!runtimeReadable(text.pointer(),text.size())){
        SKSE::log::warn("Flight sprint falling guard unavailable: executable text unreadable; existing sprint behavior retained");return;
    }
    const auto code=text.pointer<const std::uint8_t>();
    std::unordered_set<std::uintptr_t> inspected;
    std::uintptr_t site{},actionSite{};unsigned matches{},actionMatches{};
    for(std::size_t offset=0;offset+5<=text.size();++offset){
        if(code[offset]!=0xE8)continue;
        std::int32_t relative{};std::memcpy(&relative,code+offset+1,4);
        if(text.address()+offset+5+relative!=overencumbered)continue;
        const auto f=nativeFunction(text.address()+offset);
        if(!f||!inspected.insert(f->begin).second)continue;
        const auto call=runtime::sprintFallCall(f->begin,bytes(*f),fall,overencumbered,requestOffset);
        if(call.size==5){site=f->begin+call.offset;++matches;}
        const auto action=runtime::sprintActionFallCall(f->begin,bytes(*f),fall,overencumbered);
        if(action.size==5){actionSite=f->begin+action.offset;++actionMatches;}
    }
    if(matches!=1||actionMatches!=1||site==actionSite){
        SKSE::log::warn("Flight sprint falling guard unavailable: qualification matches={} SprintStart matches={} runtime={}; existing sprint behavior retained",matches,actionMatches,runtimeName());return;
    }
    callers={site+5,actionSite+5,replacementCaller(fall)};
    const auto init=MH_Initialize();
    if(init!=MH_OK&&init!=MH_ERROR_ALREADY_INITIALIZED){SKSE::log::warn("Flight sprint falling guard unavailable: MinHook initialization status={}",static_cast<int>(init));return;}
    const auto created=MH_CreateHook(reinterpret_cast<void*>(fall),reinterpret_cast<void*>(check),reinterpret_cast<void**>(&original));
    if(created!=MH_OK){original=nullptr;SKSE::log::warn("Flight sprint falling guard unavailable: hook creation status={}",static_cast<int>(created));return;}
    eligible=predicate;
    const auto enabled=MH_EnableHook(reinterpret_cast<void*>(fall));
    if(enabled!=MH_OK){MH_RemoveHook(reinterpret_cast<void*>(fall));original=nullptr;SKSE::log::warn("Flight sprint falling guard unavailable: hook activation status={}",static_cast<int>(enabled));return;}
    SKSE::log::info("Flight sprint falling guard installed: revision=dive-sprint-4 runtime={} qualificationReturn={:X} SprintStartReturn={:X} replacementReturn={:X}; original predicate retained for all other callers and actors",runtimeName(),callers[0],callers[1],callers[2]);
}
}
