#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wingflight::diagnostic {
// Policy code remains engine-independent. Reports observe existing decisions;
// no callback can change a guard result, sample, record or flight state.
struct Event {
    std::string_view code, action, subject;
    std::uintptr_t context{};
    std::optional<double> actual, expected;
};
using Observer = void (*)(const Event&) noexcept;
inline std::atomic<Observer> observer{};
inline void report(std::string_view code, std::string_view action,
    const void* context=nullptr, std::string_view subject={},
    std::optional<double> actual={}, std::optional<double> expected={}) noexcept {
    if(const auto sink=observer.load(std::memory_order_relaxed))
        sink({code,action,subject,reinterpret_cast<std::uintptr_t>(context),actual,expected});
}
constexpr std::uint64_t hash(std::string_view text) noexcept {
    std::uint64_t value=14695981039346656037ULL;
    for(const auto c:text){value^=static_cast<unsigned char>(c);value*=1099511628211ULL;}
    return value;
}
// Fixed storage: no retained game pointers, strings or growing per-frame maps.
// Once full, recycle the oldest key so a new failure can still be reported.
template<std::size_t Capacity=512> class Ledger {
    static_assert(Capacity>0);
    struct Key {
        std::uint64_t code{},subject{};
        std::uintptr_t context{};
        bool used{};
    };
    std::array<Key,Capacity> keys_{};
    std::size_t cursor_{};
public:
    bool first(const Event& event) noexcept {
        const Key key{hash(event.code),hash(event.subject),event.context,true};
        for(const auto& existing:keys_)
            if(existing.used&&existing.code==key.code&&existing.subject==key.subject
                &&existing.context==key.context)return false;
        keys_[cursor_]=key;cursor_=(cursor_+1)%Capacity;return true;
    }
    void reset() noexcept {keys_={};cursor_=0;}
};
}

namespace wingflight {
void installDiagnosticLog();
void resetDiagnosticLog() noexcept;
}
