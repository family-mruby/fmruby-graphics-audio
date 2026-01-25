# Source files - conditional compilation based on target
set(SRCS
    "main_new.c"
    "lgfx_wrapper.cpp"
    "audio.cpp"
    "common/fmrb_link_cobs.c"
    "handlers/graphics_handler.cpp"
    "handlers/audio_handler.c"
    "tasks/graphics_task.c"
    "tasks/comm_task.c"
    "tasks/audio_task.c"
)

# Display abstraction layer
if(CONFIG_IDF_TARGET_LINUX)
    list(APPEND SRCS "display/display_sdl2.cpp")
    list(APPEND SRCS "communication/comm_socket.c")
    list(APPEND SRCS "handlers/input_handler.c")
    list(APPEND SRCS "handlers/input_socket.c")
else()
    list(APPEND SRCS "display/display_esp32.cpp")
    list(APPEND SRCS "communication/comm_spi.c")
endif()

# Include directories
set(INCLUDE_DIRS
    "."
    "common"
    "communication"
    "display"
    "handlers"
    "tasks"
    "../components/apu_emu/src"
)

# Required components
set(REQUIRES
    LovyanGFX
    apu_emu
)

# Private requirements
set(PRIV_REQUIRES
    spi_flash
)

# Linux-specific requirements
if(CONFIG_IDF_TARGET_LINUX)
    list(APPEND REQUIRES msgpack)
endif()

idf_component_register(
    SRCS ${SRCS}
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES ${PRIV_REQUIRES}
    INCLUDE_DIRS ${INCLUDE_DIRS}
)

# Linux-specific: Link SDL2 library
if(CONFIG_IDF_TARGET_LINUX)
    target_link_libraries(${COMPONENT_LIB} INTERFACE SDL2)
endif()
