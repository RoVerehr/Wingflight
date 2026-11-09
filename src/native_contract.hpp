#pragma once
#include <cstdint>
#include <span>
#include <cstring>
#include <optional>
#include <functional>
#include <vector>

namespace wingflight::runtime {
struct NativeContract {
    bool pointerTime{};
    bool keys{},count{},initialized{};
    bool generatorLayout() const{return keys&&count&&initialized;}
};
NativeContract inspectContract(std::span<const std::uint8_t> code);
// Identify the native directional falling predicate, not a generic midair check.
bool fallingPredicate(std::span<const std::uint8_t> code,std::uint32_t processOffset);
struct SprintFallCall {std::size_t offset{};std::uintptr_t target{};unsigned size{};};
SprintFallCall sprintFallCall(std::uintptr_t address,std::span<const std::uint8_t> code,
    std::uintptr_t falling,std::uintptr_t overencumbered,std::uint32_t requestFlagOffset);
// The action dispatcher independently rejects ActionSprintStart while falling.
// Keep this distinct from the per-player sprint-request qualification above.
SprintFallCall sprintActionFallCall(std::uintptr_t address,std::span<const std::uint8_t> code,
    std::uintptr_t falling,std::uintptr_t overencumbered);
std::vector<SprintFallCall> directCalls(std::uintptr_t address,std::span<const std::uint8_t> code);
using PointerReader=std::function<std::optional<std::uintptr_t>(std::uintptr_t)>;
// Recognize a dispatched sprint replacement by its actual native predicate,
// stamina requirement, request cancellation and sprint-change callback.
SprintFallCall replacementSprintFallCall(std::uintptr_t address,std::span<const std::uint8_t> code,
    std::uintptr_t falling,std::uintptr_t sprintChanged,const PointerReader& pointer);
struct UnwindFunction {std::uint32_t begin{},end{},unwind{};};
static_assert(sizeof(UnwindFunction)==12);
struct FunctionBounds {std::uintptr_t begin{},end{};};
// Windows may return only one shrink-wrapped unwind fragment. Follow its
// CHAININFO owner, then join contiguous fragments belonging to that same owner.
// Lookup returns an optional UnwindFunction; info returns checked unwind bytes.
template<class Lookup,class Info> std::optional<FunctionBounds> nativeFunctionBounds(
    std::uintptr_t address,std::uintptr_t base,Lookup lookup,Info info){
    const auto rootOf=[&](UnwindFunction entry)->std::optional<UnwindFunction>{
        for(unsigned depth=0;depth<16;++depth){
            if(entry.end<=entry.begin)return {};
            const auto bytes=info(entry.unwind);
            if(bytes.size()<4||(bytes[0]&7)==0||(bytes[0]&7)>2)return {};
            if(!(bytes[0]&0x20))return entry; // UNW_FLAG_CHAININFO << 3
            const std::size_t offset=4+((static_cast<unsigned>(bytes[2])+1)&~1U)*2;
            if(offset+sizeof(UnwindFunction)>bytes.size())return {};
            UnwindFunction parent;std::memcpy(&parent,bytes.data()+offset,sizeof(parent));
            if(parent.begin>entry.begin)return {};
            entry=parent;
        }
        return {}; // Malformed or cyclic chain.
    };
    const auto initial=lookup(address);if(!initial)return {};
    const auto root=rootOf(*initial);if(!root)return {};
    auto end=root->end;
    for(unsigned count=0;count<64;++count){
        if(end-root->begin>0x10000)return {};
        const auto next=lookup(base+end);
        const auto nextRoot=next?rootOf(*next):std::optional<UnwindFunction>{};
        if(!next||next->begin!=end||!nextRoot||nextRoot->begin!=root->begin||nextRoot->unwind!=root->unwind){
            if(address<base+root->begin||address>=base+end)return {};
            return FunctionBounds{base+root->begin,base+end};
        }
        if(next->end<=end)return {};
        end=next->end;
    }
    return {};
}
}
