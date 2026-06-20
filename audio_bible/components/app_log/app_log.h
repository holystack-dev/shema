// app_log — persistent ESP_LOG capture to the SD card, for post-mortem debug.
//
// All ESP_LOG output is mirrored to /sdcard/LOG.TXT so intermittent issues
// (which can take a long time to reproduce) can be inspected after the fact,
// even untethered. Overflow is bounded twice over: a fixed PSRAM ring buffer
// that overwrites its oldest bytes, and an SD file that rotates to LOG.OLD past
// a size cap. Call app_log_init() once, after the SD card is mounted.
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the log hook + start the background flush task. Safe to call once,
// after /sdcard is mounted. No-op (returns ESP_OK) if already initialised.
esp_err_t app_log_init(void);

// Force the in-RAM ring to be written to SD now (e.g. just before power-off).
void app_log_flush(void);

#ifdef __cplusplus
}
#endif
