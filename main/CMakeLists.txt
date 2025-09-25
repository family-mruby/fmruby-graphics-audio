idf_component_register(
  SRCS "main.c" "lgfx_wrapper.cpp"
  REQUIRES LovyanGFX
  INCLUDE_DIRS ""
)

#set(image ../storage)
#fatfs_create_rawflash_image(storage ${image} FLASH_IN_PROJECT PRESERVE_TIME)
