# ========== ORIGINAL CONTENT COMMENTED OUT ==========
# # Source files - conditional compilation based on target
# set(SRCS
#     "common/fmrb_link_cobs.c"
#     "handlers/graphics_handler.cpp"
#     "tasks/graphics_task.c"
#     "tasks/comm_task.c"
#     "tasks/audio_task.c"
# )
#
# # Main file selection based on target
# if(CONFIG_IDF_TARGET_LINUX)
#     list(APPEND SRCS "main_test.cpp")  # Minimal test for Linux
# else()
#     list(APPEND SRCS "main_new.cpp")   # Full implementation for ESP32
# endif()
#
# # Add ESP32-specific sources
# if(NOT CONFIG_IDF_TARGET_LINUX)
#     list(APPEND SRCS "lgfx_wrapper.cpp")
#     list(APPEND SRCS "audio.cpp")
# endif()
#
# # Display and audio abstraction layer
# if(CONFIG_IDF_TARGET_LINUX)
#     list(APPEND SRCS "display/display_sdl2.cpp")
#     list(APPEND SRCS "communication/comm_socket.c")
#     list(APPEND SRCS "handlers/input_handler.c")
#     list(APPEND SRCS "handlers/input_socket.c")
#     list(APPEND SRCS "handlers/audio_handler_sdl2.c")
# else()
#     list(APPEND SRCS "display/display_esp32.cpp")
#     list(APPEND SRCS "communication/comm_spi.c")
#     list(APPEND SRCS "handlers/audio_handler_esp32.c")
# endif()
#
# # Include directories
# set(INCLUDE_DIRS
#     "."
#     "common"
#     "communication"
#     "display"
#     "handlers"
#     "tasks"
# )
#
# # Add apu_emu include directory only for ESP32
# if(NOT CONFIG_IDF_TARGET_LINUX)
#     list(APPEND INCLUDE_DIRS "../components/apu_emu/src")
# endif()
#
# # Required components
# set(REQUIRES
#     LovyanGFX
#     msgpack-esp32
#     esp_timer
#     spiffs
# )
#
# # Add apu_emu requirement only for ESP32
# if(NOT CONFIG_IDF_TARGET_LINUX)
#     list(APPEND REQUIRES apu_emu)
# endif()
#
# # Private requirements
# set(PRIV_REQUIRES
#     spi_flash
# )
#
# idf_component_register(
#     SRCS ${SRCS}
#     REQUIRES ${REQUIRES}
#     PRIV_REQUIRES ${PRIV_REQUIRES}
#     INCLUDE_DIRS ${INCLUDE_DIRS}
# )
#
# # Linux-specific: Link SDL2 library and define LGFX_USE_SDL
# if(CONFIG_IDF_TARGET_LINUX)
#     target_link_libraries(${COMPONENT_LIB} INTERFACE SDL2)
#     target_compile_definitions(${COMPONENT_LIB} PRIVATE LGFX_USE_SDL)
# endif()
# ========== END OF COMMENTED OUT CONTENT ==========

# ========== BUILD CONFIGURATION ==========
# Build main.cpp which includes platform-specific implementation

set(SRCS
    "main.cpp"
)

# Add SDL platform implementation for Linux
if(CONFIG_IDF_TARGET_LINUX)
    list(APPEND SRCS
        "../components/LovyanGFX/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp"
        "../components/LovyanGFX/src/lgfx/v1/platforms/sdl/common.cpp"
    )
endif()

set(INCLUDE_DIRS
    "."
)

set(REQUIRES
    LovyanGFX
)

idf_component_register(
    SRCS ${SRCS}
    REQUIRES ${REQUIRES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
)

# Linux-specific: Link SDL2 library and define LGFX_SDL
if(CONFIG_IDF_TARGET_LINUX)
    target_link_libraries(${COMPONENT_LIB} INTERFACE SDL2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LGFX_SDL)
endif()
