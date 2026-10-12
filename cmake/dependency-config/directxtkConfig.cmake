if(NOT TARGET Microsoft::DirectXTK)
  message(FATAL_ERROR "Wingflight must add DirectXTK before CommonLibSSE")
endif()
