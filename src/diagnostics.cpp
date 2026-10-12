#include "diagnostics.hpp"
#include <SKSE/SKSE.h>
#include <mutex>

namespace wingflight {
namespace {
std::mutex diagnosticMutex;
diagnostic::Ledger<> ledger;
void emit(const diagnostic::Event& event) noexcept {
    // Reporting must never turn a recoverable invalid sample into an exception.
    try {
        {std::lock_guard lock(diagnosticMutex);if(!ledger.first(event))return;}
        const auto actual=event.actual?fmt::format("{:.6g}",*event.actual):"n/a";
        const auto expected=event.expected?fmt::format("{:.6g}",*event.expected):"n/a";
        SKSE::log::warn("Diagnostic failure: reason={} context={:X} subject={} actual={} expected={} action={} (repeats suppressed for this context)",
            event.code,event.context,event.subject,actual,expected,event.action);
    } catch(...) {}
}
}
void installDiagnosticLog(){diagnostic::observer.store(emit,std::memory_order_release);}
void resetDiagnosticLog() noexcept {
    try {std::lock_guard lock(diagnosticMutex);ledger.reset();}catch(...) {}
}
}
