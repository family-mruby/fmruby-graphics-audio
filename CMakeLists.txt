cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fmruby-graphics-audio)

# Generate Flash data
spiffs_create_partition_image(audio storage FLASH_IN_PROJECT)
#fatfs_create_rawflash_image(storage fatfs FLASH_IN_PROJECT PRESERVE_TIME)
