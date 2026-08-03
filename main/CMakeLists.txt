set(SRCS
    "main.cpp"
    "tasks/audio_task.c"
    "audio/audio_latency.c"
    "tasks/comm_task.c"
    "tasks/message_handler_task.c"
    "tasks/graphics_task.cpp"
    "common/fmrb_link_cobs.c"
    "common/fmrb_link_fragment.c"
    "common/fmrb_bmp332.c"
    "mempool/fmrb_mempool.c"
    "mempool/fmrb_sprite_pool.c"
    "mempool/tlsf/tlsf.c"
    "graphics/sprite_manager.cpp"
    "graphics/gfx_vm.cpp"
    "file_transfer/file_transfer_handler.c"
)

# Add platform-specific sources
if(CONFIG_IDF_TARGET_LINUX)
    # Linux headless platform (SDL2 is in a separate process)
    list(APPEND SRCS
        "graphics/graphics_handler.cpp"
        "graphics/display_shm.cpp"
        "audio/audio_handler_shm.c"
        "input_linux/input_handler_ipc.c"
        "input_linux/input_socket.c"
        "communication/comm_socket_server.c"
        "communication/fmrb_link_msgpack.c"
    )
else()
    # ESP32 platform implementation
    set(FMRB_COMM_TRANSPORT "UART" CACHE STRING "Comm transport: SPI, UART")
    list(APPEND SRCS
        "graphics/graphics_handler.cpp"
        "graphics/display_cvbs.cpp"
        "graphics/lgfx_test.cpp"
        "audio/audio_handler_esp32.c"
        "audio/audio_check.c"
        "communication/fmrb_link_msgpack.c"
    )
    if(FMRB_COMM_TRANSPORT STREQUAL "UART")
        list(APPEND SRCS "communication/comm_uart_slave.c")
        message(STATUS "Comm transport: UART")
    else()
        list(APPEND SRCS "communication/comm_spi_slave.c")
        message(STATUS "Comm transport: SPI")
    endif()
endif()

set(INCLUDE_DIRS
    "."
    "audio"
    "common"
    "communication"
    "graphics"
    "include"
    "input_linux"
    "mempool"
    "mempool/tlsf"
    "tasks"
    "file_transfer"
)

set(REQUIRES
    LovyanGFX
    fmrb_pin_assign
    # esp_app_desc.h: the boot screen reports which build it is (version, build
    # time, IDF). Available on the Linux target too, so it is not ESP32 only.
    esp_app_format
)

# Add apu_emu for all targets
list(APPEND REQUIRES apu_emu)

# Add ESP32-specific requirements
if(NOT CONFIG_IDF_TARGET_LINUX)
    list(APPEND REQUIRES
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

# Linux-specific: Link shared memory and realtime libraries (no SDL2)
if(CONFIG_IDF_TARGET_LINUX)
    target_link_libraries(${COMPONENT_LIB} INTERFACE rt pthread)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LGFX_LINUX_FB)
endif()
