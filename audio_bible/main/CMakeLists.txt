# `main` implicitly depends on all other components, so the vendored BSP
# components (i2c_bsp, sdcard_bsp, codec_board, ...) are visible here and
# can pull user_config.h from this directory via their PRIV_REQUIRES main.
idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS ".")
