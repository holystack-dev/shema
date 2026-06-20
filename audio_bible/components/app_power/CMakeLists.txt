idf_component_register(
  SRCS "app_power.c"
  INCLUDE_DIRS "."
  REQUIRES adc_bsp button_bsp lcd_bl_pwm_bsp app_store app_player app_log
  PRIV_REQUIRES driver esp_timer esp_hw_support main)
