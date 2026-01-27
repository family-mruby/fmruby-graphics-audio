# Set required components based on target
if (IDF_TARGET STREQUAL "linux")
    # For Linux, create empty component (not used)
    idf_component_register(
        INCLUDE_DIRS "include"
    )
    return()
else()
    set(APU_REQUIRES
        "spiffs"
        "driver"
        "esp_partition"
        "esp_timer"
        "freertos"
        "esp_system"
        "soc"
        "esp_hw_support"
        "spi_flash"
    )

    idf_component_register(
        SRCS "src/apu_if.cpp"
             "src/nofrendo/nes_apu.c"
        INCLUDE_DIRS
            "include"
        PRIV_INCLUDE_DIRS
            "src"
            "src/nofrendo"
        REQUIRES ${APU_REQUIRES}
    )

    target_compile_options(__idf_apu_emu PRIVATE
        -w  # Disable all warnings
        -Wno-error  # Don't treat warnings as errors
        -Wno-xor-used-as-pow  # Disable XOR-as-power warnings
        -Wno-volatile  # Disable volatile warnings
        -Wno-implicit-fallthrough  # Disable fallthrough warnings
        -Wno-attributes  # Disable attribute warnings
        -Wno-missing-field-initializers  # Disable struct initializer warnings
        -Wno-deprecated-declarations  # Disable deprecated API warnings
    )
endif()