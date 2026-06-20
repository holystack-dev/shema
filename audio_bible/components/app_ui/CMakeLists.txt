idf_component_register(
  SRCS "app_ui.c"
  INCLUDE_DIRS "."
  REQUIRES lvgl app_player app_store app_power
  PRIV_REQUIRES main)
