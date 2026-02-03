set(SRCS
    "main.cpp"
)

# Add platform-specific sources
if(CONFIG_IDF_TARGET_LINUX)
    # Linux/SDL platform implementation
    list(APPEND SRCS
        "../components/LovyanGFX/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp"
        "../components/LovyanGFX/src/lgfx/v1/platforms/sdl/common.cpp"
        "handlers/graphics_handler.cpp"
        "handlers/audio_handler_sdl2.c"
        "handlers/input_handler.c"
        "handlers/input_socket.c"
        "communication/comm_socket.c"
        "common/fmrb_link_cobs.c"
    )
else()
    # ESP32 platform implementation
    list(APPEND SRCS
        "lgfx_wrapper.cpp"
        "audio.cpp"
        "communication/comm_spi.c"
    )
endif()

set(INCLUDE_DIRS
    "."
    "communication"
    "handlers"
    "common"
)

# Add apu_emu include directory only for ESP32
if(NOT CONFIG_IDF_TARGET_LINUX)
    list(APPEND INCLUDE_DIRS "../components/apu_emu/src")
endif()

set(REQUIRES
    LovyanGFX
)

# Add ESP32-specific requirements
if(NOT CONFIG_IDF_TARGET_LINUX)
    list(APPEND REQUIRES
        apu_emu
        msgpack-esp32
        esp_littlefs
    )
endif()

idf_component_register(
    SRCS ${SRCS}
    REQUIRES ${REQUIRES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
)

# Linux-specific: Link SDL2 library and define LGFX_USE_SDL
if(CONFIG_IDF_TARGET_LINUX)
    target_link_libraries(${COMPONENT_LIB} INTERFACE SDL2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LGFX_USE_SDL)
    target_include_directories(${COMPONENT_LIB} PRIVATE /usr/include/SDL2)
endif()
