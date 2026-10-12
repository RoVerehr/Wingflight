if(NOT TARGET spdlog::spdlog)
  message(FATAL_ERROR "Wingflight must add spdlog before CommonLibSSE")
endif()
