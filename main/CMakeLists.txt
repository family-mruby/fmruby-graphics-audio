set(SRCS
    "main.cpp"
    "tasks/audio_task.c"
    "tasks/comm_task.c"
    "tasks/message_handler.c"
    "tasks/graphics_task.cpp"
    "common/fmrb_link_cobs.c"
)

# Add platform-specific sources
if(CONFIG_IDF_TARGET_LINUX)
    # Linux/SDL platform implementation
    list(APPEND SRCS
        "graphics/graphics_handler.cpp"
        "graphics/display_sdl2.cpp"
        "audio/audio_handler_sdl2.c"
        "input_linux/input_handler.c"
        "input_linux/input_socket.c"
        "communication/comm_socket_server.c"
        "communication/fmrb_link_msgpack.c"
        "communication/message_queue.c"
    )
else()
    # ESP32 platform implementation
    list(APPEND SRCS
        "graphics/graphics_handler.cpp"
        "graphics/display_cvbs.cpp"
        "graphics/lgfx_test.cpp"
        "audio/audio_check.c"
        "communication/comm_spi_slave.c"
        "communication/fmrb_link_msgpack.c"
        "communication/message_queue.c"
    )
endif()

set(INCLUDE_DIRS
    "."
    "audio"
    "common"
    "communication"
    "graphics"
    "input_linux"
    "tasks"
)

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

# Test mode flags
option(ENABLE_SPI_TEST "Enable SPI communication test" OFF)
option(ENABLE_GRAPHICS_TEST "Enable graphics test" OFF)

if(ENABLE_SPI_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ENABLE_SPI_TEST)
    message(STATUS "SPI test enabled")
endif()

if(ENABLE_GRAPHICS_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ENABLE_GRAPHICS_TEST)
    message(STATUS "Graphics test enabled")
endif()

# Linux-specific: Link SDL2 library and define LGFX_USE_SDL
if(CONFIG_IDF_TARGET_LINUX)
    target_link_libraries(${COMPONENT_LIB} INTERFACE SDL2)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LGFX_USE_SDL)
    target_include_directories(${COMPONENT_LIB} PRIVATE /usr/include/SDL2)
endif()
