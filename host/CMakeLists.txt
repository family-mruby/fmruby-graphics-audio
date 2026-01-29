cmake_minimum_required(VERSION 3.16)
project(fmrb_graphics_audio_host)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build option for IPC debug logging
option(FMRB_IPC_DEBUG "Enable IPC message dump logging" ON)

add_definitions(-DLGFX_SDL)

# Add IPC debug definition if enabled
if(FMRB_IPC_DEBUG)
    add_definitions(-DFMRB_IPC_DEBUG)
    message(STATUS "IPC debug logging enabled")
endif()

# Find SDL2 and msgpack
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(MSGPACK REQUIRED msgpack)

# LovyanGFX path
set(LOVYANGFX_PATH ../components/LovyanGFX)

# Include directories
include_directories(
    include
    ../main
    ../main/common
    ../main/handlers
    ../main/communication
    ../main/display
    ../main/tasks
    ${SDL2_INCLUDE_DIRS}
    ${MSGPACK_INCLUDE_DIRS}
    ${LOVYANGFX_PATH}/src
)

# LovyanGFX source files
file(GLOB LOVYANGFX_SOURCES
    ${LOVYANGFX_PATH}/src/lgfx/Fonts/efont/*.c
    ${LOVYANGFX_PATH}/src/lgfx/Fonts/IPA/*.c
    ${LOVYANGFX_PATH}/src/lgfx/utility/*.c
    ${LOVYANGFX_PATH}/src/lgfx/v1/*.cpp
    ${LOVYANGFX_PATH}/src/lgfx/v1/misc/*.cpp
    ${LOVYANGFX_PATH}/src/lgfx/v1/panel/Panel_Device.cpp
    ${LOVYANGFX_PATH}/src/lgfx/v1/panel/Panel_FrameBufferBase.cpp
    ${LOVYANGFX_PATH}/src/lgfx/v1/platforms/sdl/*.cpp
)

# Source files
set(SOURCES
    src/main.cpp
    ../main/handlers/graphics_handler.cpp
    ../main/handlers/audio_handler_sdl2.c
    ../main/handlers/input_handler.c
    ../main/handlers/input_socket.c
    ../main/communication/comm_socket.c
    ../main/common/fmrb_link_cobs.c
    ${LOVYANGFX_SOURCES}
)

# Create executable
add_executable(fmrb_graphics_audio_host ${SOURCES})

# Link libraries
target_link_libraries(fmrb_graphics_audio_host
    ${SDL2_LIBRARIES}
    ${MSGPACK_LIBRARIES}
    pthread
    m
)

# Compiler flags
target_compile_options(fmrb_graphics_audio_host PRIVATE ${SDL2_CFLAGS_OTHER})

# Install target
install(TARGETS fmrb_graphics_audio_host
    RUNTIME DESTINATION bin
)

# Print build information
message(STATUS "Building fmrb_graphics_audio_host with SDL2 and LovyanGFX")
message(STATUS "SDL2 libraries: ${SDL2_LIBRARIES}")
message(STATUS "SDL2 include dirs: ${SDL2_INCLUDE_DIRS}")
