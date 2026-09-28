# lcd_init.c and drv/tca9554.c are omitted: on this board the LCD is driven by the
# app's own esp_lcd path (main.c) and the TCA9554 expander by i2c_bsp, so those files
# are unused. Only the ES8311 output path
# (cfg_parse/codec_board/codec_init/dummy_codec) is used.

# esp_codec_dev is public (exposed by include/codec_init.h); the rest are private,
# used only inside the .c files.
set(COMPONENT_PRIV_REQUIRES driver fatfs)

if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_GREATER_EQUAL "5.3")
list(APPEND COMPONENT_PRIV_REQUIRES esp_driver_i2c)
endif()

idf_component_register(SRCS  "cfg_parse.c" "codec_board.c" "codec_init.c" "dummy_codec.c"
                       INCLUDE_DIRS ./include
                       REQUIRES esp_codec_dev
                       PRIV_REQUIRES "${COMPONENT_PRIV_REQUIRES}"
                       EMBED_TXTFILES "board_cfg.txt")
