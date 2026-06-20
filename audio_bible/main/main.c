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

// Resume-from-last instead of autoplay: the player stays stopped on boot and the
// now-playing bar offers "Resume <book> <chapter>" (tap to continue from the last
// saved position). Set to 1 only for a hands-free audio self-test.
#define AUTOPLAY_ON_BOOT 0

static const char *TAG = "main";

#define LCD_BIT_PER_PIXEL 16
#define LCD_OPCODE_READ_CMD (0x0BULL)

static SemaphoreHandle_t lvgl_mux;
static SemaphoreHandle_t lvgl_flush_sem;
static uint16_t *lvgl_dma_buf;
static esp_lcd_panel_handle_t g_panel;   // for panel sleep (disp on/off)
static volatile bool g_disp_sleeping;    // true => screen off, panel blanked, skip flush

QueueHandle_t app_touch_data_queue;

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
    lv_disp_flush_ready(drv);
}

// ---- Touch (CST816S, I2C @0x15 on the shared bus) ----
// Data register 0x02 returns: [num, xH(low4), xL, yH(low4), yL].
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
        app_touch_t t = {.x = x, .y = y};
        xQueueSend(app_touch_data_queue, &t, 0);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

// Global tap feedback: a short click on every completed press. Fires only on
// real CLICKED events (not scrolls, and not the swallowed wake-tap).
static void lvgl_feedback_cb(lv_indev_drv_t *drv, uint8_t event)
{
    (void)drv;
    if (event == LV_EVENT_CLICKED) app_player_click();
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
    if (esp_lcd_panel_io_rx_param(io, rd_cmd, id, sizeof(id)) == ESP_OK)
        ESP_LOGI(TAG, "ST77916 ID(0x04): %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
    else
        ESP_LOGW(TAG, "ST77916 ID read failed");
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
    indev_drv.feedback_cb = lvgl_feedback_cb;   // tap click sound
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

    app_touch_data_queue = xQueueCreate(10, sizeof(app_touch_t));
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);

    i2c_master_Init();   // single shared I2C bus + TCA9554 expander — first
    display_init();
    xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", 6 * 1024, NULL, 4, NULL, 0);

    app_store_init();    // NVS (settings, favourites, resume)
    app_power_init();    // amp (GPIO15), ADC, buttons, backlight
    _sdcard_init();      // mount /sdcard (FAT32)
    app_log_init();      // mirror ESP_LOG -> /sdcard/LOG.TXT (post-mortem debug)
    sd_list_root();

    if (app_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
    }

    // Power-saving hooks: blank the panel on screen auto-off, and gate the
    // speaker amp on playback state (amp on before audio, off when idle).
    app_power_set_display_sleep_cb(bible_display_sleep);
    app_player_set_amp_cb(app_power_set_amp);

    if (bible_lvgl_lock(-1)) {
        bible_app_start();
        bible_lvgl_unlock();
    }

    // Power management from saved settings (Settings screen). Screen auto-off and
    // deep-sleep idle power-off (the latter only while nothing is playing).
    app_power_set_screen_timeout(app_store_get_screen_timeout(30));
    int poweroff_s = app_store_get_poweroff(300);
    app_power_set_idle_off(poweroff_s > 0, poweroff_s > 0 ? poweroff_s : 300);

#if AUTOPLAY_ON_BOOT
    vTaskDelay(pdMS_TO_TICKS(500));
    app_player_set_volume(70);
    app_player_play(0, 1);   // Genesis 1 — auto-advances through the book
    ESP_LOGI(TAG, "autoplay: requested Genesis 1");
#endif
}
