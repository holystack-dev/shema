# Audio Bible — Waveshare ESP32-S3-Touch-LCD-1.85C V2 (round 360x360)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# On-demand UI screenshots, for documentation and bug reports:
#
#   BIBLE_SCREENSHOTS=1 idf.py reconfigure build flash
#   python tools/screenshot_server.py <port> <outdir> <ctldir>
#
# `reconfigure` is required: the variable is read at CMake configure time.
#
# Off in normal builds. Keep it off for power testing: it installs the USB-Serial-JTAG
# driver (see main.c).
project(audio_bible)

# Defined only for main and app_ui, so switching modes does not rebuild the SDK.
if("$ENV{BIBLE_SCREENSHOTS}" STREQUAL "1")
  idf_component_get_property(shema_main main COMPONENT_LIB)
  idf_component_get_property(shema_ui app_ui COMPONENT_LIB)
  target_compile_definitions(${shema_main} PRIVATE BIBLE_SCREENSHOTS=1)
  target_compile_definitions(${shema_ui} PRIVATE BIBLE_SCREENSHOTS=1)
endif()
