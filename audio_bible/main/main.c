// Audio Bible — display + touch + LVGL bring-up for the
// Waveshare ESP32-S3-Touch-LCD-1.85C V2 (ST77916 QSPI round 360x360, CST816S touch).
//
// Ported from the 3.49" bar-LCD build (AXS15231B, 172x640). Panel reset is via
// the TCA9554 expander (EXIO2); touch reset via EXIO1. The init sequence follows
// the Waveshare ST77916 reference (variant probe on register 0x04).
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "lvgl.h"
#include "esp_lcd_st77916.h"
#include <dirent.h>

#include "i2c_bsp.h"
#include "user_config.h"
#include "bsp_lvgl.h"
#include "sdcard_bsp.h"
#include "app_player.h"
#include "app_store.h"
#include "app_power.h"
#include "app_log.h"
#include "app_played.h"

// Resume-from-last instead of autoplay: the player stays stopped on boot and the
// now-playing bar offers "Resume <book> <chapter>" (tap to continue from the last
// saved position). Set to 1 only for a hands-free audio self-test.
#define AUTOPLAY_ON_BOOT 0

static const char *TAG = "main";

#define LCD_BIT_PER_PIXEL 16
#define LCD_OPCODE_READ_CMD (0x0BULL)
#define LCD_OPCODE_WRITE_CMD (0x02ULL)

static SemaphoreHandle_t lvgl_mux;
static SemaphoreHandle_t lvgl_flush_sem;
static uint16_t *lvgl_dma_buf;
static esp_lcd_panel_handle_t g_panel;   // for panel sleep (disp on/off)
static esp_lcd_panel_io_handle_t g_panel_io;  // for the raw SLPIN before deep sleep
static volatile bool g_disp_sleeping;    // true => screen off, panel blanked, skip flush

// Waveshare ST77916 vendor init sequence (round 1.85" panel). Used when the
// panel-variant probe on register 0x04 reports the "case 2" signature.
static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0}, {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0}, {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0}, {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0}, {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0}, {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0}, {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0}, {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0}, {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0}, {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0}, {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0}, {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0}, {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0}, {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0}, {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0}, {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0}, {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0}, {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0}, {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0}, {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0}, {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0}, {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0}, {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0}, {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0}, {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0}, {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0}, {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0}, {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0}, {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0}, {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0}, {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0}, {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0}, {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0}, {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0}, {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0}, {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0}, {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0}, {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0}, {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0}, {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0}, {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0}, {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0}, {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0}, {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0}, {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0}, {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0}, {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0}, {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0}, {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0}, {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0}, {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0}, {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0}, {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0}, {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0}, {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0}, {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0}, {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0}, {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0}, {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0}, {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0}, {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0}, {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0}, {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0}, {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0}, {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0}, {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0}, {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0}, {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0}, {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0}, {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0}, {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0}, {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0}, {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0}, {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0}, {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0}, {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0}, {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0}, {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0}, {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0}, {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0}, {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0}, {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0}, {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0}, {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0}, {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0}, {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0}, {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0}, {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0}, {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0}, {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 0},
};

// ---- LVGL lock (exported) ----
bool bible_lvgl_lock(int timeout_ms)
{
    const TickType_t ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, ticks) == pdTRUE;
}

void bible_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

// Screen auto-off: blank the panel (DISPOFF) and skip the LVGL flush work, then
// repaint on wake. Taken under the LVGL lock so the panel command / repaint
// can't race the flush. The ST77916 disp_on_off bool is the normal sense
// (true => display ON), so sleep maps to !on.
void bible_display_sleep(bool sleep)
{
    bible_lvgl_lock(-1);
    if (sleep != g_disp_sleeping) {
        g_disp_sleeping = sleep;
        if (g_panel) esp_lcd_panel_disp_on_off(g_panel, !sleep);
        if (!sleep) lv_obj_invalidate(lv_scr_act());  // force full repaint on wake
    }
    bible_lvgl_unlock();
}

// Deep power-down for the panel, for the deep-sleep path only.
//
// Sends Sleep In (0x10) to stop the ST77916 oscillator and booster, which DISPOFF
// leaves running. The next boot's init sequence issues SLPOUT (0x11).
//
// QSPI framing matches the st77916 driver's tx_param(): opcode 0x02 in bits
// 31:24, the command byte in bits 15:8.
void bible_display_power_down(void)
{
    if (!g_panel_io) return;
    bible_lvgl_lock(-1);
    g_disp_sleeping = true;                                   // no further flush work
    if (g_panel) esp_lcd_panel_disp_on_off(g_panel, false);   // 0x28 DISPOFF
    esp_err_t rc = esp_lcd_panel_io_tx_param(g_panel_io,
                                             (int)((LCD_OPCODE_WRITE_CMD << 24) | (0x10 << 8)),
                                             NULL, 0);        // 0x10 SLPIN
    bible_lvgl_unlock();
    if (rc != ESP_OK) ESP_LOGW(TAG, "panel SLPIN failed (%s)", esp_err_to_name(rc));
    vTaskDelay(pdMS_TO_TICKS(120));   // datasheet: allow the booster to ramp down
}

// ---- Boot splash ----
// Shown as soon as LVGL is up and left on screen while the SD card mounts and the
// player initialises, so it covers real boot latency instead of adding a delay. The
// first nav_reset() in bible_app_start() replaces it.
extern const uint8_t splash_bin_start[] asm("_binary_splash_bin_start");
extern const uint8_t splash_bin_end[]   asm("_binary_splash_bin_end");

#define SPLASH_MIN_MS 2000        // minimum time the splash stays up, measured from boot

static uint32_t splash_t0;

static void show_splash(void)
{
    size_t len = (size_t)(splash_bin_end - splash_bin_start);
    size_t want = (size_t)EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2;
    if (len < want) {                      // truncated asset: skip rather than read past it
        ESP_LOGW(TAG, "splash is %u B, expected %u — skipping", (unsigned)len, (unsigned)want);
        return;
    }
    static lv_img_dsc_t dsc;
    dsc.header.cf     = LV_IMG_CF_TRUE_COLOR;
    dsc.header.always_zero = 0;
    dsc.header.w      = EXAMPLE_LCD_H_RES;
    dsc.header.h      = EXAMPLE_LCD_V_RES;
    dsc.data_size     = want;
    dsc.data          = splash_bin_start;   // flash-mapped; no RAM copy

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, &dsc);
    lv_obj_center(img);
    splash_t0 = esp_log_timestamp();
}

// Keep the splash visible for at least SPLASH_MIN_MS from creation. The backlight comes
// up in app_power_init(), after the splash is created; slow boots pay nothing extra.
static void splash_hold(void)
{
    uint32_t elapsed = esp_log_timestamp() - splash_t0;
    if (elapsed < SPLASH_MIN_MS) {
        ESP_LOGI(TAG, "splash: holding a further %u ms", (unsigned)(SPLASH_MIN_MS - elapsed));
        vTaskDelay(pdMS_TO_TICKS(SPLASH_MIN_MS - elapsed));
    }
}

// ---- On-demand screenshot capture (dev builds only: BIBLE_SCREENSHOTS=1) ----
//
// The draw buffer is a full 360x360 frame (full_refresh = 1), so the flush callback's
// color_map is the finished screen; it is streamed as base64 and reassembled into a PNG
// by tools/screenshot_server.py. Drive it with: SHOT <name> on stdin.
//
// Installs the USB-Serial-JTAG driver for stdin; plugging/unplugging the charger can
// reset the board on this build. Do not use it for power testing.
#if BIBLE_SCREENSHOTS
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
static volatile bool g_shot_req;
static SemaphoreHandle_t g_shot_done;
static char g_shot_name[32];

void bible_display_preview_splash(void)
{
    lv_obj_clean(lv_scr_act());
    show_splash();
}

static const char SHOT_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#include "capture_video.inc"

// Every payload line carries an "S:" prefix so the host can pick the frame out of
// interleaved ESP_LOG output rather than assuming it gets a clean stream.
static void shot_emit(const uint8_t *px, size_t len)
{
    printf("\n<<<SHOT %s %d %d %u>>>\n", g_shot_name,
           EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, (unsigned)len);
    fflush(stdout);
    char line[2 + 128 + 2];
    size_t i = 0;
    while (i < len) {
        int n = 0;
        line[n++] = 'S';
        line[n++] = ':';
        for (int g = 0; g < 32 && i < len; g++) {   // 32 groups = 96 bytes = 128 chars
            uint32_t v = 0;
            int have = 0;
            for (int k = 0; k < 3; k++) {
                v <<= 8;
                if (i < len) { v |= px[i++]; have++; }
            }
            line[n++] = SHOT_B64[(v >> 18) & 63];
            line[n++] = SHOT_B64[(v >> 12) & 63];
            line[n++] = have > 1 ? SHOT_B64[(v >> 6) & 63] : '=';
            line[n++] = have > 2 ? SHOT_B64[v & 63] : '=';
        }
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
    }
    fflush(stdout);
    printf("<<<ENDSHOT %s>>>\n", g_shot_name);
    fflush(stdout);
}

// Force a full repaint and block until that frame has been streamed out. Called from the
// capture task, never from the LVGL task — it must not hold the LVGL lock while it
// waits, since the flush it is waiting on needs that lock.
void bible_display_capture(const char *name)
{
    if (!g_shot_done) return;
    snprintf(g_shot_name, sizeof g_shot_name, "%s", name);
    xSemaphoreTake(g_shot_done, 0);          // drop any stale completion
    bible_lvgl_lock(-1);
    lv_obj_invalidate(lv_scr_act());
    g_shot_req = true;
    bible_lvgl_unlock();
    if (xSemaphoreTake(g_shot_done, pdMS_TO_TICKS(10000)) != pdTRUE)
        ESP_LOGW(TAG, "screenshot '%s' timed out", name);
}

static void screenshot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));         // let boot, SD mount and the first paint settle

    // The operator drives the UI by hand and the host asks for a frame whenever the
    // screen shows what it wants. Keep the panel lit and block the idle power-off, or a
    // session dies part-way through and captures black frames.
    app_power_set_screen_timeout(0);
    app_power_set_idle_off(false, 0);
    app_power_wake();

    // stdin is not readable until the USB-JTAG driver backs the console VFS; the
    // default path returns EOF immediately. Installing it also makes stdout blocking
    // and buffered, which is strictly safer for the 350 KB base64 dumps.
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&ucfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed; manual capture unavailable");
        vTaskDelete(NULL);
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    ESP_LOGW(TAG, "<<<SHOT READY>>> send: SHOT <name>, SCENE <name>, SCROLL <y>");

    char cmd[64];
    for (;;) {
        if (!fgets(cmd, sizeof cmd, stdin)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        char *nl = strpbrk(cmd, "\r\n");
        if (nl) *nl = 0;
        if (strcmp(cmd, "PING") == 0) {
            printf("<<<SHOT READY>>>\n");
            fflush(stdout);
            continue;
        }
        if (strcmp(cmd, "VIDEO START") == 0 || strcmp(cmd, "VIDEO STOP") == 0) {
            video_command(strcmp(cmd, "VIDEO START") == 0);
            continue;
        }
        if (strncmp(cmd, "TOUCH ", 6) == 0) {
            int x, y, down;
            bool ok = sscanf(cmd + 6, "%d %d %d", &x, &y, &down) == 3;
            bible_lvgl_lock(-1);
            if (ok) ok = bible_ui_capture_pointer(x, y, down != 0);
            bible_lvgl_unlock();
            printf("<<<INPUT %s>>>\n", ok ? "OK" : "ERROR"); fflush(stdout);
            continue;
        }
        if (strncmp(cmd, "SCENE ", 6) == 0 || strncmp(cmd, "SCROLL ", 7) == 0 || strncmp(cmd, "GLIDE ", 6) == 0) {
            bible_lvgl_lock(-1);
            bool ok = true;
            if (cmd[2] == 'E') ok = bible_ui_capture_scene(cmd + 6);
            else {
                int y = 0;
                bool animated = cmd[0] == 'G';
                if (sscanf(cmd + (animated ? 6 : 7), "%d", &y) == 1) bible_ui_capture_scroll(y, animated);
                else ok = false;
            }
            int y, max_y, height;
            bible_ui_capture_metrics(&y, &max_y, &height);
            bible_lvgl_unlock();
            // Allow the next UI tick/paint to settle before accepting a SHOT.
            vTaskDelay(pdMS_TO_TICKS(450));
            printf("<<<UI %s %d %d %d>>>\n", ok ? "OK" : "UNAVAILABLE", y, max_y, height);
            fflush(stdout);
            continue;
        }
        if (strncmp(cmd, "SHOT ", 5) != 0) continue;
        const char *name = cmd + 5;
        if (!*name) name = "manual";
        bible_display_capture(name);
    }
}
#endif

// ---- Display flush ----
static bool notify_flush_ready(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(lvgl_flush_sem, &woken);
    return woken == pdTRUE;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    if (g_disp_sleeping) { lv_disp_flush_ready(drv); return; }  // screen off: skip QSPI work
    const int flush_count = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (EXAMPLE_LCD_V_RES / flush_count);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int y1 = 0, y2 = offgap;
    uint16_t *map = (uint16_t *)color_map;

    // Drain any stale transfer-done signal so a prior desync can't corrupt this
    // flush's give/take accounting — makes the loop self-correcting frame to frame.
    while (xSemaphoreTake(lvgl_flush_sem, 0) == pdTRUE) { }

    // One DMA buffer is reused per chunk, so each draw must finish before the next
    // memcpy. The wait uses a finite timeout (not portMAX_DELAY): if a transfer
    // completion is ever lost, the LVGL task recovers on the next frame instead of
    // blocking forever with the screen lit but UI frozen until a reboot.
    for (int i = 0; i < flush_count; i++) {
        memcpy(lvgl_dma_buf, map, LVGL_DMA_BUFF_LEN);
        if (esp_lcd_panel_draw_bitmap(panel, 0, y1, EXAMPLE_LCD_H_RES, y2, lvgl_dma_buf) != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap failed at chunk %d/%d", i, flush_count);
            break;
        }
        if (xSemaphoreTake(lvgl_flush_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "flush wait timed out at chunk %d/%d (recovering)", i, flush_count);
            break;
        }
        y1 += offgap;
        y2 += offgap;
        map += dmalen;
    }
#if BIBLE_SCREENSHOTS
    video_submit((const uint8_t *)color_map);
    if (g_shot_req) {
        g_shot_req = false;
        shot_emit((const uint8_t *)color_map, LVGL_SPIRAM_BUFF_LEN);
        if (g_shot_done) xSemaphoreGive(g_shot_done);
    }
#endif
    lv_disp_flush_ready(drv);
}

// ---- Touch (CST816S, I2C @0x15 on the shared bus) ----
// Data register 0x02 returns: [num, xH(low4), xL, yH(low4), yL].
#if BIBLE_SCREENSHOTS
static void trace_touch(bool down, int x, int y, const char *reason)
{
    static bool active;
    static int last_x, last_y;
    static uint32_t started;
    if (down) {
        if (!active) {
            started = lv_tick_get();
            ESP_LOGI(TAG, "TOUCH raw down %d,%d", x, y);
        }
        last_x = x; last_y = y;
    } else if (active) {
        ESP_LOGI(TAG, "TOUCH raw up %d,%d after %lu ms (%s)", last_x, last_y,
                 (unsigned long)(lv_tick_get() - started), reason);
    }
    active = down;
}
#else
#define trace_touch(down, x, y, reason) ((void)0)
#endif
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    // Touch does NOT wake the screen (the BOOT button does). While the panel is
    // asleep, skip polling entirely.
    if (app_power_screen_is_off()) { data->state = LV_INDEV_STATE_REL; return; }

    // Poll the data register every frame for snappy response. The CST816S only
    // *pulses* its INT line per report (it doesn't hold it low during a press),
    // so gating reads on the INT level drops most touches. Instead we read every
    // frame and treat a NACK (controller doesn't ACK when no finger is down) as
    // "released". The idle NACKs are harmless — their i2c logging is silenced in
    // app_main so they don't spam the console.
    uint8_t buff[6] = {0};
    if (i2c_read_buff(disp_touch_dev_handle, CST816S_REG_TOUCH_DATA, buff, 5) != ESP_OK) {
        trace_touch(false, 0, 0, "i2c");
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    uint8_t fingers = buff[0] & 0x0f;
    if (fingers > 0 && fingers < 3) {
        uint16_t x = (((uint16_t)(buff[1] & 0x0f)) << 8) | buff[2];
        uint16_t y = (((uint16_t)(buff[3] & 0x0f)) << 8) | buff[4];
        if (x >= EXAMPLE_LCD_H_RES) x = EXAMPLE_LCD_H_RES - 1;
        if (y >= EXAMPLE_LCD_V_RES) y = EXAMPLE_LCD_V_RES - 1;
        app_power_user_activity();           // keep the screen awake while in use
        data->state = LV_INDEV_STATE_PR;
        data->point.x = x;
        data->point.y = y;
        trace_touch(true, x, y, "contact");
    } else {
        trace_touch(false, 0, 0, "no contact");
        data->state = LV_INDEV_STATE_REL;
    }
}

// No explicit touch power-down before deep sleep: with DisAutoSleep (0xFE) = 0 from
// reset, the CST816S enters its low-power mode after AutoSleepTime (0xF9) with no touch,
// which is also why it NACKs the polling reads above when idle.

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    uint32_t delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (bible_lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            bible_lvgl_unlock();
        }
        if (delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        else if (delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void display_init(void)
{
    lvgl_flush_sem = xSemaphoreCreateBinary();

    // Hardware-reset the panel (EXIO2) and the touch controller (EXIO1) via the
    // TCA9554 expander, which i2c_master_Init() has already brought up.
    ESP_LOGI(TAG, "LCD/touch reset via TCA9554");
    expander_reset_pulse(EXIO_LCD_RST);
    expander_reset_pulse(EXIO_TOUCH_RST);

    // Keep the touch INT (GPIO4) as a pulled-up input so the CST816S INT line
    // doesn't float. We do NOT gate reads on it (it only pulses per report).
    gpio_config_t tint = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_TOUCH_INT),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&tint);

    ESP_LOGI(TAG, "Init QSPI bus");
    spi_bus_config_t buscfg = {
        .data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0,
        .data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1,
        .data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2,
        .data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3,
        .sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK,
        .max_transfer_sz = LVGL_DMA_BUFF_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Probe the panel variant at low speed (read register 0x04) to decide whether
    // the Waveshare vendor init sequence is needed (mirrors the reference driver).
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 3 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io));

    uint8_t id[4] = {0};
    int rd_cmd = (0x04 & 0xff) << 8;
    rd_cmd |= LCD_OPCODE_READ_CMD << 24;
    // Retry once: if this probe fails, a "case 2" panel silently gets the driver-default
    // init (wrong gamma / garbled) for the whole boot, so it's worth a second attempt.
    esp_err_t idrc = esp_lcd_panel_io_rx_param(io, rd_cmd, id, sizeof(id));
    if (idrc != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        idrc = esp_lcd_panel_io_rx_param(io, rd_cmd, id, sizeof(id));
    }
    if (idrc == ESP_OK)
        ESP_LOGI(TAG, "ST77916 ID(0x04): %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    else
        ESP_LOGW(TAG, "ST77916 ID read failed (falling back to default init)");
    ESP_ERROR_CHECK(esp_lcd_panel_io_del(io));

    // Reopen at full speed with the flush-done callback for LVGL.
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.on_color_trans_done = notify_flush_ready;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io));

    st77916_vendor_config_t vendor_cfg = {
        .flags = { .use_qspi_interface = 1 },
    };
    // "case 2" panels (id == 00 02 7F 7F) need the explicit vendor sequence.
    if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        vendor_cfg.init_cmds = vendor_specific_init_new;
        vendor_cfg.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(st77916_lcd_init_cmd_t);
        ESP_LOGI(TAG, "ST77916: using Waveshare vendor init");
    } else {
        ESP_LOGI(TAG, "ST77916: using driver default init");
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,                    // reset handled via EXIO2 above
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .flags = { .reset_active_high = 0 },
        .vendor_config = &vendor_cfg,
    };
    ESP_LOGI(TAG, "Install ST77916 panel");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io, &panel_cfg, &panel));
    g_panel = panel;
    g_panel_io = io;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "Init LVGL");
    lv_init();
    lvgl_dma_buf = heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
    assert(lvgl_dma_buf);
    lv_color_t *buf1 = heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    assert(buf1);

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1; // must be 1 for the chunked flush above
    disp_drv.user_data = panel;
    lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {.callback = lvgl_tick_cb, .name = "lvgl_tick"};
    esp_timer_handle_t tick = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);
}

static void sd_list_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { ESP_LOGW(TAG, "cannot open %s", path); return; }
    ESP_LOGI(TAG, "%s contents:", path);
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL && n < 30) { ESP_LOGI(TAG, "  %s", e->d_name); n++; }
    if (n == 0) ESP_LOGW(TAG, "  (empty)");
    closedir(d);
}

static void sd_list_root(void)
{
    sd_list_dir("/sdcard");
    sd_list_dir("/sdcard/AUDIO");
}

void app_main(void)
{
    // The CST816S NACKs I2C reads when no finger is down (we poll it every
    // frame), so silence the i2c driver's per-read error logging.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

    i2c_master_Init();   // single shared I2C bus + TCA9554 expander — first
    display_init();
    show_splash();       // up before the slow init below, so the wait is not a black screen
    // 8 KB: the LVGL task runs full rendering, deep page builds, ui_tick's
    // NVS commit, FATFS opendir, and the app_log vprintf hook's stack buffer.
    xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", 8 * 1024, NULL, 4, NULL, 0);

    app_store_init();    // NVS (settings, favourites, resume)
    app_power_init();    // amp (GPIO15), ADC, buttons, backlight
    _sdcard_init();      // mount /sdcard (FAT32)
    app_log_init();      // mirror ESP_LOG -> /sdcard/LOG.TXT (post-mortem debug)
    app_played_init();   // per-track progress from /sdcard/<collection>/played.txt
    // Log the reset reason and wake cause (deep-sleep wake vs brownout/watchdog reset).
    ESP_LOGI(TAG, "boot: reset_reason=%d wakeup_cause=%d",
             (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause());
    sd_list_root();

    if (app_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
    }

    // Power-saving hooks: blank the panel on screen auto-off, and gate the
    // speaker amp on playback state (amp on before audio, off when idle).
    app_power_set_display_sleep_cb(bible_display_sleep);
    app_power_set_display_powerdown_cb(bible_display_power_down);
    app_player_set_amp_cb(app_power_set_amp);

    // Hold the splash for the rest of its minimum time, then show the UI.
    splash_hold();

    if (bible_lvgl_lock(-1)) {
        bible_app_start();
        bible_lvgl_unlock();
    }

    // Power management from saved settings (Settings screen). Screen auto-off and
    // deep-sleep idle power-off (the latter only while nothing is playing).
    app_power_set_screen_timeout(app_store_get_screen_timeout(30));
    int poweroff_s = app_store_get_poweroff(300);
    app_power_set_idle_off(poweroff_s > 0, poweroff_s > 0 ? poweroff_s : 300);

#if BIBLE_SCREENSHOTS
    g_shot_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(screenshot_task, "shots", 8 * 1024, NULL, 3, NULL, 0);
#endif

#if AUTOPLAY_ON_BOOT
    vTaskDelay(pdMS_TO_TICKS(500));
    app_player_set_volume(70);
    app_player_play(0, 1);   // Genesis 1 — auto-advances through the book
    ESP_LOGI(TAG, "autoplay: requested Genesis 1");
#endif
}
