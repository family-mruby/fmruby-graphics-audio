# Platform-specific requirements for GPIO pin definitions
if(IDF_TARGET STREQUAL "linux")
    # Linux doesn't need GPIO driver
    set(PLATFORM_REQUIRES log)
else()
    # ESP32: GPIO driver for GPIO_NUM_* macros
    set(PLATFORM_REQUIRES driver log)
endif()

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES ${PLATFORM_REQUIRES}
)
