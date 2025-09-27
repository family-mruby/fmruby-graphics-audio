idf_component_register(
  SRCS "main.c" "lgfx_wrapper.cpp" "audio.cpp"
  REQUIRES LovyanGFX apu_emu
  PRIV_REQUIRES spi_flash
  INCLUDE_DIRS "." "../components/apu_emu/src"
)
