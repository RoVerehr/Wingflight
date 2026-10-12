#pragma once
#include <cstdint>
#include <span>

namespace wingflight::runtime {
struct NativeContract {
    bool pointerTime{};
    bool keys{},count{},initialized{};
    bool generatorLayout() const{return keys&&count&&initialized;}
};
NativeContract inspectContract(std::span<const std::uint8_t> code);
}
