// app_power — battery, buttons, brightness, power-off / idle auto-off.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*power_btn_cb_t)(void);
typedef void (*display_sleep_cb_t)(bool sleep);

// Inits TCA9554 (power latch + speaker amp), ADC (battery), buttons, backlight.
// Must be called after i2c_master_Init() and after app_store_init().
void app_power_init(void);

int   app_power_battery_pct(void);     // 0..100
float app_power_battery_volts(void);

void app_power_set_brightness(int pct); // 0..100 (persisted by caller)

void app_power_off(void);                       // latch off + deep sleep (PWR wakes)
void app_power_set_idle_off(bool enable, int timeout_s);
// Reset the screen idle timer on user action. Does NOT wake the screen — touch
// no longer wakes; the sun/BOOT button does (app_power_wake).
void app_power_user_activity(void);
bool app_power_screen_is_off(void);              // true while panel is asleep
void app_power_wake(void);                       // turn the screen back on
void app_power_set_screen_timeout(int seconds);  // 0 = never

// BOOT button single-click -> typically "back"/"home" in the UI.
void app_power_set_boot_cb(power_btn_cb_t cb);

// Speaker amp (TCA9554 pin 7). Driven on only while audio plays to save power
// and avoid idle hiss; wire as the player's amp callback.
void app_power_set_amp(bool on);

// Hook to blank/restore the display on screen auto-off (e.g. bible_display_sleep).
void app_power_set_display_sleep_cb(display_sleep_cb_t cb);

#ifdef __cplusplus
}
#endif
