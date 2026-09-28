idf_component_register(
  SRCS "app_player.c" "ui_click.c"
  INCLUDE_DIRS "."
  REQUIRES esp_codec_dev codec_board bible_data app_played
  PRIV_REQUIRES esp-libhelix-mp3)
