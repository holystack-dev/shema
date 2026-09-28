idf_component_register(
  SRCS "sdcard_bsp.c"
  REQUIRES sdmmc                          # public: sdcard_bsp.h includes driver/sdmmc_host.h
  PRIV_REQUIRES fatfs
  INCLUDE_DIRS "./")