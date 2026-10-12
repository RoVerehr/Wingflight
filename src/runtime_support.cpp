#include "runtime_support.hpp"
#include "native_contract.hpp"
#include "runtime_diagnostic.hpp"
#include "runtime_callable.hpp"
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <limits>

namespace wingflight {
runtime::Backend runtimeBackend() {
    const auto version=REL::Module::get().version();
    return runtime::select(version[0],version[1],version[2],version[3]);
}
const char* runtimeName(){return runtime::name(runtimeBackend());}
bool runtimeReadable(const void* pointer,std::size_t bytes) {
    auto begin=reinterpret_cast<std::uintptr_t>(pointer);
    if(!begin||!bytes||bytes>std::numeric_limits<std::uintptr_t>::max()-begin)return false;
    const auto end=begin+bytes;
    while(begin<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(begin),&info,sizeof(info))||info.State!=MEM_COMMIT
            ||(info.Protect&(PAGE_NOACCESS|PAGE_GUARD))
            ||!(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
        const auto next=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=begin)return false;begin=next;
    }
    return true;
}
bool runtimeExecutable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return address&&VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))
        &&info.State==MEM_COMMIT&&!(info.Protect&PAGE_GUARD)
        &&(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))
        &&info.AllocationBase==reinterpret_cast<void*>(REL::Module::get().base());
}
namespace {
const char* entryName(std::uint64_t id) {
    switch(id) {
    case 72576:return "NiMultiTargetTransformController::Update";
    case 62932:return "BGSGamebryoSequenceGenerator::Update";
    case 70191:return "NiObject::CreateDeepCopy";
    case 72783:return "NiTransformInterpolator::Update";
    default:return "unknown";
    }
}
std::string moduleName(std::uintptr_t address) {
    HMODULE module{};
    if(!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module))return "<not a loaded module>";
    std::array<wchar_t,2048> path{};
    const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!length||length>=path.size())return "<module name unavailable>";
    return SKSE::stl::utf16_to_utf8(std::filesystem::path(path.data()).filename().native()).value_or("<module name unavailable>");
}
std::string entryBytes(std::uintptr_t address,std::size_t count) {
    if(!runtimeReadable(reinterpret_cast<void*>(address),count))return "<unreadable>";
    const auto bytes=reinterpret_cast<const std::uint8_t*>(address);
    std::string result;
    for(std::size_t i=0;i<count;++i){if(i)result+=' ';result+=fmt::format("{:02X}",bytes[i]);}
    return result;
}
void logEntryTransfer(const char* name,std::uintptr_t address) {
    if(!runtimeReadable(reinterpret_cast<void*>(address),16))return;
    const auto transfer=runtime::inspectEntryTransfer(address,{reinterpret_cast<const std::uint8_t*>(address),16});
    if(transfer.kind==runtime::EntryTransferKind::none)return;
    if(transfer.kind==runtime::EntryTransferKind::breakpoint) {
        SKSE::log::warn("Wing runtime diagnostic: entry={} starts with a breakpoint instruction",name);return;
    }
    auto destination=transfer.address;
    if(transfer.kind==runtime::EntryTransferKind::indirectJumpSlot) {
        if(!runtimeReadable(reinterpret_cast<void*>(destination),sizeof(destination))) {
            SKSE::log::warn("Wing runtime diagnostic: entry={} jump slot={:X} is unreadable",name,destination);return;
        }
        std::memcpy(&destination,reinterpret_cast<void*>(destination),sizeof(destination));
    }
    SKSE::log::warn("Wing runtime diagnostic: entry={} first jump destination={:X} destinationModule={}; observation only",
        name,destination,moduleName(destination));
}
bool callableEntry(std::uintptr_t actual,std::uint64_t id) {
    const auto selectedId=runtime::wingEntryId(runtimeBackend(),id);
    const auto expected=REL::ID(selectedId).address();
    const auto base=REL::Module::get().base();
    const auto name=entryName(id);
    SKSE::log::info("Wing runtime diagnostic: entry={} id={} actual={:X} expected={:X} actualModule={} expectedModule={} expectedRVA={:X} actualBytes=[{}] expectedBytes=[{}]",
        name,selectedId,actual,expected,moduleName(actual),moduleName(expected),expected-base,entryBytes(actual,16),entryBytes(expected,16));
    if(!runtime::executableMemory(actual)||!runtimeReadable(reinterpret_cast<void*>(actual),16)) {
        SKSE::log::error("Wing runtime validation failed: entry={} reason=not-callable executableMemory={} readable16={}",
            name,runtime::executableMemory(actual),runtimeReadable(reinterpret_cast<void*>(actual),16));
        logEntryTransfer(name,actual);return false;
    }
    if(actual!=expected||!runtimeExecutable(actual)) {
        SKSE::log::warn("Wing runtime compatibility: entry={} has an external callback; continuing through the current function, without replacing its code",name);
    }
    logEntryTransfer(name,actual);
    return true;
}
void observeLegacyPrefix(std::uintptr_t address,std::uintptr_t rva,std::initializer_list<std::uint8_t> bytes) {
    const bool accepted=address==REL::Module::get().base()+rva
        &&std::memcmp(reinterpret_cast<void*>(address),bytes.begin(),bytes.size())==0;
    if(!accepted) {
        std::string expected;
        for(const auto byte:bytes){if(!expected.empty())expected+=' ';expected+=fmt::format("{:02X}",byte);}
        SKSE::log::warn("Wing runtime compatibility: observed native prefix differs for runtime {}; continuing. expectedRVA={:X} actual={:X} addressMatches={} expectedBytes=[{}] actualBytes=[{}]",
            runtimeName(),rva,address,address==REL::Module::get().base()+rva,expected,entryBytes(address,bytes.size()));
    }
}
}
bool verifyWingEntrypoints(std::uintptr_t multi,std::uintptr_t generator,std::uintptr_t copy,std::uintptr_t sampler) {
    SKSE::log::info("Wing runtime validation started: runtime={} diagnosticRevision=hook-compatible-1",runtimeName());
    if(!runtime::supported(runtimeBackend())||!callableEntry(multi,72576)||!callableEntry(generator,62932)
        ||!callableEntry(copy,70191)||!callableEntry(sampler,72783)){
        if(!runtime::supported(runtimeBackend()))SKSE::log::error("Wing runtime validation rejected: unsupported runtime layout={}",runtimeName());
        return false;}
    if(runtimeReadable(reinterpret_cast<void*>(multi),512)&&runtimeReadable(reinterpret_cast<void*>(generator),1024)) {
        const auto controller=runtime::inspectContract({reinterpret_cast<const std::uint8_t*>(multi),512});
        const auto events=runtime::inspectContract({reinterpret_cast<const std::uint8_t*>(generator),1024});
        SKSE::log::info("Wing runtime diagnostic: controllerPointerTime={} generatorKeys={} generatorCount={} generatorInitialized={}",
            controller.pointerTime,events.keys,events.count,events.initialized);
        if(!controller.pointerTime||!events.generatorLayout()) {
            SKSE::log::warn("Wing runtime compatibility: static instruction features differ; continuing with supported runtime layouts and live object checks");
        }
    } else {
        SKSE::log::warn("Wing runtime diagnostic: extended instruction scan unavailable; continuing with callable entries and live object checks");
    }
    if(runtimeBackend()==runtime::Backend::steam1597) {
        observeLegacyPrefix(multi,0xCCD660,{0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x20});
        observeLegacyPrefix(generator,0xAD1060,{0x48,0x8B,0xC4,0x48,0x81,0xEC,0x98,0,0,0});
        observeLegacyPrefix(copy,0xC529A0,{0x40,0x53,0x56,0x57,0x48,0x81,0xEC,0x50,0x06,0,0});
        observeLegacyPrefix(sampler,0xCD2C40,{0x48,0x89,0x5C,0x24,0x10,0x57,0x48,0x83,0xEC,0x60});
    } else if(runtime::legacy(runtimeBackend())) {
        observeLegacyPrefix(multi,0xD98C60,{0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x20});
        observeLegacyPrefix(generator,0xB92C30,{0x48,0x8B,0xC4,0x48,0x81,0xEC,0x98,0,0,0});
        observeLegacyPrefix(copy,0xD18080,{0x40,0x53,0x56,0x57,0x48,0x81,0xEC,0x50,0x06,0,0});
        observeLegacyPrefix(sampler,0xD9E9A0,{0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x83,0xEC,0x60});
    }
    SKSE::log::info("Wing runtime validation passed: runtime={} callable functions accepted; third-party hooks and signature differences are diagnostic only",runtimeName());
    return true;
}
}
