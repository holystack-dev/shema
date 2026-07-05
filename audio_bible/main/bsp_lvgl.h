#pragma once
// Shared LVGL access for the app layer: the LVGL APIs are NOT thread-safe, so
// every task that touches LVGL must hold this lock. The display/touch port
// lives in main.c; the app builds its screens under this lock.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Take/release the global LVGL mutex. timeout_ms = -1 blocks forever.
bool bible_lvgl_lock(int timeout_ms);
void bible_lvgl_unlock(void);

// Implemented by the app layer; called once after display + LVGL are up,
// while the LVGL lock is held.
void bible_app_start(void);

// Blank (sleep=true) or restore the panel for screen auto-off. Safe to call
// from any task — serialised against the LVGL flush via the LVGL lock.
void bible_display_sleep(bool sleep);

#ifdef __cplusplus
}
#endif
