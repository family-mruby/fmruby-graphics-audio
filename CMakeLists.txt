cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fmruby-graphics-audio)

# Generate Flash data (ESP32 only)
if (NOT IDF_TARGET STREQUAL "linux")
    littlefs_create_partition_image(storage flash FLASH_IN_PROJECT)
endif()
