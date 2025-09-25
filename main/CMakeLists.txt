idf_component_register(
  SRCS "main.c"
  #REQUIRES picoruby-esp32 m5unified
  #PRIV_REQUIRES spi_flash
  INCLUDE_DIRS ""
)

#set(image ../storage)
#fatfs_create_rawflash_image(storage ${image} FLASH_IN_PROJECT PRESERVE_TIME)
