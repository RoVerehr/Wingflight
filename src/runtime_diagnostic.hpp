#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace wingflight::runtime {
// Describes one entry transfer for logging only. Never follows or installs it.
enum class EntryTransferKind { none, relativeJump, indirectJumpSlot, absoluteJump, breakpoint };
struct EntryTransfer {
    EntryTransferKind kind{EntryTransferKind::none};
    std::uintptr_t address{};
};
inline EntryTransfer inspectEntryTransfer(std::uintptr_t entry,std::span<const std::uint8_t> bytes) {
    if(bytes.empty())return {};
    if(bytes[0]==0xCC)return {EntryTransferKind::breakpoint,0};
    if(bytes[0]==0xE9&&bytes.size()>=5) {
        std::int32_t displacement{};std::memcpy(&displacement,bytes.data()+1,sizeof(displacement));
        return {EntryTransferKind::relativeJump,entry+5+static_cast<std::uintptr_t>(displacement)};
    }
    if(bytes[0]==0xEB&&bytes.size()>=2)
        return {EntryTransferKind::relativeJump,entry+2+static_cast<std::uintptr_t>(static_cast<std::int8_t>(bytes[1]))};
    if(bytes.size()>=6&&bytes[0]==0xFF&&bytes[1]==0x25) {
        std::int32_t displacement{};std::memcpy(&displacement,bytes.data()+2,sizeof(displacement));
        return {EntryTransferKind::indirectJumpSlot,entry+6+static_cast<std::uintptr_t>(displacement)};
    }
    if(bytes.size()>=12&&bytes[0]==0x48&&bytes[1]==0xB8&&bytes[10]==0xFF&&bytes[11]==0xE0) {
        std::uint64_t destination{};std::memcpy(&destination,bytes.data()+2,sizeof(destination));
        return {EntryTransferKind::absoluteJump,static_cast<std::uintptr_t>(destination)};
    }
    return {};
}
}
