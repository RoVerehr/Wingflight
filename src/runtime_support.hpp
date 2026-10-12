#pragma once
#include "runtime_policy.hpp"
#include <cstddef>
#include <cstdint>

namespace wingflight {
runtime::Backend runtimeBackend();
const char* runtimeName();
bool runtimeReadable(const void* pointer,std::size_t bytes);
bool runtimeExecutable(std::uintptr_t address);
bool verifyWingEntrypoints(std::uintptr_t multi,std::uintptr_t generator,std::uintptr_t copy,std::uintptr_t sampler);
}
