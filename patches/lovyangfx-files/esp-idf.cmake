# CMakeLists for ESP-IDF

set(COMPONENT_ADD_INCLUDEDIRS
    ${LGFX_ROOT}/src
    )

# Check if building for Linux target
if (IDF_TARGET STREQUAL "linux")
    # Linux (SDL2) specific sources
    # Note: Panel files are in platforms/sdl/ directory, not panel/
    file(GLOB SRCS
         ${LGFX_ROOT}/src/lgfx/Fonts/efont/*.c
         ${LGFX_ROOT}/src/lgfx/Fonts/IPA/*.c
         ${LGFX_ROOT}/src/lgfx/utility/*.c
         ${LGFX_ROOT}/src/lgfx/v1/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/misc/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/platforms/sdl/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/touch/*.cpp
         )
    # Add Panel base classes needed by Panel_sdl
    list(APPEND SRCS ${LGFX_ROOT}/src/lgfx/v1/panel/Panel_Device.cpp)
    list(APPEND SRCS ${LGFX_ROOT}/src/lgfx/v1/panel/Panel_FrameBufferBase.cpp)
    set(COMPONENT_REQUIRES)
else()
    # ESP32 specific sources
    file(GLOB SRCS
         ${LGFX_ROOT}/src/lgfx/Fonts/efont/*.c
         ${LGFX_ROOT}/src/lgfx/Fonts/IPA/*.c
         ${LGFX_ROOT}/src/lgfx/utility/*.c
         ${LGFX_ROOT}/src/lgfx/v1/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/misc/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/panel/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/platforms/arduino_default/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/platforms/esp32/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/platforms/esp32c3/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/platforms/esp32s2/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/platforms/esp32s3/*.cpp
         ${LGFX_ROOT}/src/lgfx/v1/touch/*.cpp
         )
    if (IDF_VERSION_MAJOR GREATER_EQUAL 5)
        set(COMPONENT_REQUIRES nvs_flash efuse esp_lcd driver esp_timer)
    elseif ((IDF_VERSION_MAJOR EQUAL 4) AND (IDF_VERSION_MINOR GREATER 3) OR IDF_VERSION_MAJOR GREATER 4)
        set(COMPONENT_REQUIRES nvs_flash efuse esp_lcd)
    else()
        set(COMPONENT_REQUIRES nvs_flash efuse)
    endif()
endif()

set(COMPONENT_SRCS ${SRCS})


### If you use arduino-esp32 components, please activate next comment line.
# list(APPEND COMPONENT_REQUIRES arduino-esp32)


message(STATUS "LovyanGFX use components = ${COMPONENT_REQUIRES}")

register_component()

# Add platform-specific compile definitions after registration
if (IDF_TARGET STREQUAL "linux")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LGFX_USE_SDL)
    # Link SDL2 library for Linux builds
    # SDL2 is linked in main component, so just add include path here
    target_include_directories(${COMPONENT_LIB} PRIVATE /usr/include/SDL2)
    # Suppress specific warnings that occur in LovyanGFX on Linux builds
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-maybe-uninitialized)
endif()
