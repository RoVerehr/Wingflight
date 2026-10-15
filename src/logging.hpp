#pragma once
#include <atomic>
#include <spdlog/spdlog.h>

namespace wingflight::logging {
inline std::atomic_bool detailedEnabled{};
inline bool detailed(){return detailedEnabled.load(std::memory_order_relaxed);}
inline void configure(bool enabled){
    // Info is reserved for initialization and compatibility decisions. Runtime
    // observations use Debug; warnings/errors always pass either setting.
    if(const auto logger=spdlog::default_logger()){
        logger->set_level(enabled?spdlog::level::debug:spdlog::level::info);
        logger->flush_on(enabled?spdlog::level::debug:spdlog::level::info);
        logger->flush();
    }
    detailedEnabled.store(enabled,std::memory_order_relaxed);
}
}
