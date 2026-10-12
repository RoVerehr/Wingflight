#pragma once
#include <Windows.h>
#include <cstdint>

namespace wingflight::runtime {
// Executable memory can belong to the game, another DLL or a generated relay.
// Opcode signatures and image ownership are observations, not call eligibility.
inline bool executableMemory(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return address&&VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))
        &&info.State==MEM_COMMIT&&!(info.Protect&(PAGE_GUARD|PAGE_NOACCESS))
        &&(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
}
