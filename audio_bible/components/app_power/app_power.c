#include "app_power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "user_config.h"
#include "adc_bsp.h"
#include "button_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "app_store.h"
#include "app_player.h"
#include "app_log.h"

static const char *TAG = "power";
// 1.85C V2 has no software power latch — "power off" just deep-sleeps and the
// BOOT button (GPIO0, active-low) wakes it.
#define PWR_GPIO EXAMPLE_PIN_NUM_BOOT_BTN

static power_btn_cb_t boot_cb;
static volatile bool   idle_off_en;
static volatile int    idle_timeout_s = 300;
static volatile uint64_t last_activity_ms;
static int             cur_brightness = 80;
static volatile bool   screen_off;
static volatile int    screen_timeout_s = 30;   // auto screen-off; 0 = never
static display_sleep_cb_t disp_sleep_cb;
// Speaker amp is on when audio plays OR the screen is on (for pop-free UI clicks);
// it drops only once the screen is off AND nothing is playing.
static volatile bool   amp_play;
static volatile bool   amp_screen = true;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void apply_backlight(int pct) { setUpduty(pct * 255 / 100); }  // active-high on 1.85C V2

// NS4150B speaker amp enable on GPIO15 (active-high). On the 1.85C V2 this is a
// plain GPIO, not an expander pin.
static void amp_apply(void)
{
    gpio_set_level(EXAMPLE_PIN_NUM_AMP_EN, (amp_play || amp_screen) ? 1 : 0);
}

static void screen_set_off(bool off)
{
    if (off == screen_off) return;
    screen_off = off;
    if (off) {
        setUpduty(0);                            // backlight off first (instant black)
        if (disp_sleep_cb) disp_sleep_cb(true);  // skip flush work while dark
        amp_screen = false; amp_apply();         // drop amp if nothing is playing
    } else {
        amp_screen = true; amp_apply();          // amp on before anything plays
        if (disp_sleep_cb) disp_sleep_cb(false); // restore flush
        apply_backlight(cur_brightness);         // then backlight
    }
    ESP_LOGI(TAG, "screen %s  heap int=%u dma=%u big=%u", off ? "off" : "on",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

void app_power_set_display_sleep_cb(display_sleep_cb_t cb) { disp_sleep_cb = cb; }

void app_power_set_amp(bool on) { amp_play = on; amp_apply(); }

void app_power_user_activity(void)
{
    last_activity_ms = now_ms();   // keep the screen awake; do NOT wake it from off
}

bool app_power_screen_is_off(void) { return screen_off; }

void app_power_wake(void)
{
    last_activity_ms = now_ms();
    if (screen_off) screen_set_off(false);
}

void app_power_set_screen_timeout(int seconds) { screen_timeout_s = seconds; }

float app_power_battery_volts(void)
{
    float sum = 0;
    int ok = 0;
    for (int i = 0; i < 16; i++) {       // average out ADC noise
        float v = 0;
        adc_get_value(&v, NULL);
        if (v > 0.5f) { sum += v; ok++; }
    }
    return ok ? sum / ok : 0;
}

// Single-cell Li-ion open-circuit voltage -> state of charge, in 10% steps.
// Non-linear (the discharge curve is flat in the middle), so it's far more
// accurate than a straight 3.3-4.2 V map.
static int li_ion_pct(float v)
{
    // Top saturates at ~4.10 V: the S3 ADC + ~3x divider read a fully-charged
    // pack at ~4.12-4.15 V, so a full battery now reads 100% (not ~95%). Like a
    // phone, it holds 100% across the flat top of the Li-ion curve.
    static const float curve[11] = {
        3.30f, 3.55f, 3.65f, 3.70f, 3.75f, 3.80f, 3.86f, 3.92f, 3.98f, 4.04f, 4.10f
    };
    if (v <= curve[0]) return 0;
    if (v >= curve[10]) return 100;
    for (int i = 0; i < 10; i++) {
        if (v < curve[i + 1]) {
            float frac = (v - curve[i]) / (curve[i + 1] - curve[i]);
            return (int)((i + frac) * 10.0f + 0.5f);
        }
    }
    return 100;
}

int app_power_battery_pct(void)
{
    static float ema = -1.0f;
    static int shown = -1;
    float v = app_power_battery_volts();
    if (v <= 0.5f) return shown < 0 ? 0 : shown;
    // Heavy smoothing absorbs charger plug-in spikes and load sag.
    ema = (ema < 0) ? v : (ema * 0.92f + v * 0.08f);
    int target = li_ion_pct(ema);
    // Rate-limit the displayed value: snap on first read, then creep 1% per
    // update so it never jumps (the way Android's gauge moves).
    if (shown < 0) shown = target;
    else if (target > shown) shown++;
    else if (target < shown) shown--;
    return shown;
}

void app_power_set_brightness(int pct)
{
    if (pct < 45) pct = 45;        // floor: 45% keeps the panel clearly lit
    if (pct > 100) pct = 100;
    cur_brightness = pct;
    if (!screen_off) apply_backlight(pct);
}

void app_power_off(void)
{
    ESP_LOGW(TAG, "powering off");
    app_log_flush();   // persist the tail of the log before sleeping
    amp_play = false; amp_screen = false; amp_apply();   // silence the amp
    if (disp_sleep_cb) disp_sleep_cb(true);
    setUpduty(0);                                         // backlight off
    vTaskDelay(pdMS_TO_TICKS(300));
    // No hardware latch on this board: deep-sleep and wake on BOOT (active low).
    esp_sleep_enable_ext1_wakeup(1ULL << PWR_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void app_power_set_idle_off(bool enable, int timeout_s)
{
    idle_off_en = enable;
    if (timeout_s > 0) idle_timeout_s = timeout_s;
    last_activity_ms = now_ms();
}

void app_power_set_boot_cb(power_btn_cb_t cb) { boot_cb = cb; }

static void button_task(void *arg)
{
    for (;;) {
        EventBits_t pwr = xEventGroupWaitBits(pwr_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(120));
        if (get_bit_button(pwr, 1)) {            // PWR long press -> off
            app_power_off();
        }
        EventBits_t boot = xEventGroupWaitBits(boot_groups, set_bit_all, pdTRUE, pdFALSE, 0);
        if (get_bit_button(boot, 0)) {             // sun/BOOT single click
            if (screen_off) {
                app_power_wake();                  // asleep -> just turn the screen on
            } else {
                last_activity_ms = now_ms();
                if (boot_cb) boot_cb();            // already on -> go Home
            }
        }
    }
}

static void idle_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint64_t idle = now_ms() - last_activity_ms;

        // Screen auto-off after touch inactivity — even while audio plays.
        if (screen_timeout_s > 0 && !screen_off && idle > (uint64_t)screen_timeout_s * 1000)
            screen_set_off(true);

        // Device power-off only when idle AND nothing is playing.
        if (idle_off_en) {
            player_status_t st;
            app_player_get_status(&st);
            if (st.state != PLAYER_PLAYING && idle > (uint64_t)idle_timeout_s * 1000) {
                ESP_LOGW(TAG, "idle %ds -> power off", idle_timeout_s);
                app_power_off();
            }
        }
    }
}

// Configure the speaker-amp enable line (GPIO15, active-high) and turn it on
// (screen is on at boot, so amp_screen is true).
static void amp_setup(void)
{
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_AMP_EN),
    };
    gpio_config(&io);
    amp_apply();
    ESP_LOGI(TAG, "amp enable on GPIO%d (amp %s)", EXAMPLE_PIN_NUM_AMP_EN,
             (amp_play || amp_screen) ? "on" : "off");
}

void app_power_init(void)
{
    amp_setup();
    adc_bsp_init();
    button_Init();
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
    app_power_set_brightness(app_store_get_brightness(80));
    last_activity_ms = now_ms();
    xTaskCreatePinnedToCore(button_task, "btn", 3 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(idle_task, "idle", 3 * 1024, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "power ready (batt %d%%, %.3f V)", app_power_battery_pct(), app_power_battery_volts());
}
