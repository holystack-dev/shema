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

// Blank the panel AND put the controller into Sleep In, for the deep-sleep
// power-off path. One-way: only a reboot brings the panel back.
void bible_display_power_down(void);

#if BIBLE_SCREENSHOTS
// Dev builds only. Force a full repaint and stream that frame out of the console as
// base64; blocks until it has been sent. See main.c for the wire format and the
// USB-peripheral caveat.
void bible_display_capture(const char *name);
// Development capture navigation. Caller holds the LVGL lock. These only change
// the visible page/scroll position; they never select settings or delete data.
bool bible_ui_capture_scene(const char *name);
void bible_ui_capture_scroll(int y, bool animated);
void bible_ui_capture_metrics(int *y, int *max_y, int *height);
void bible_display_preview_splash(void);
// Real LVGL pointer events for repeatable navigation recordings; may invoke actions.
bool bible_ui_capture_pointer(int x, int y, bool down);
#endif

#ifdef __cplusplus
}
#endif
