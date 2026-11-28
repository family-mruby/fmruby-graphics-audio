cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fmruby-graphics-audio)

# Generate Flash data (ESP32 only).
# The image is built from a staged copy of flash/, not flash/ itself: the
# Linux sim backs its filesystem with the working tree and writes into it
# (file transfers into cache/, apps' files into home/, the audio settings
# into etc/audio_output_linux.txt), and imaging flash/ directly took those
# into the image until it no longer fit (LFS_ERR_NOSPC on a local build,
# 3.1.0 and 3.1.1). Only what git tracks ships, as in fmruby-core: cache/
# survives with its .keep, the rest of the sim's writes are dropped.
if (NOT IDF_TARGET STREQUAL "linux")
    set(STORAGE_STAGING ${CMAKE_BINARY_DIR}/storage_root)
    add_custom_target(storage_staging
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${STORAGE_STAGING}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/flash ${STORAGE_STAGING}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${STORAGE_STAGING}/cache ${STORAGE_STAGING}/home
        COMMAND ${CMAKE_COMMAND} -E make_directory ${STORAGE_STAGING}/cache
        COMMAND ${CMAKE_COMMAND} -E touch ${STORAGE_STAGING}/cache/.keep
        COMMAND ${CMAKE_COMMAND} -E rm -f ${STORAGE_STAGING}/etc/audio_output_linux.txt
        COMMENT "Staging storage tree (the sim's writes excluded)"
    )
    littlefs_create_partition_image(storage ${STORAGE_STAGING} FLASH_IN_PROJECT DEPENDS storage_staging)
endif()
