idf_component_register(
  SRCS "i2c_bsp.c"
  REQUIRES driver espressif__esp_io_expander_tca9554
  PRIV_REQUIRES main
  INCLUDE_DIRS "./")
