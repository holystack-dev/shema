idf_component_register(
  SRCS "app_player.c"
  INCLUDE_DIRS "."
  REQUIRES esp_codec_dev codec_board
  PRIV_REQUIRES main esp-libhelix-mp3)
