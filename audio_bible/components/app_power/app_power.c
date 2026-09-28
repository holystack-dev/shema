#include "app_power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "user_config.h"
#include "adc_bsp.h"
#include "button_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "app_store.h"
#include "app_player.h"
#include "app_log.h"
#include "app_played.h"
#include "sdcard_bsp.h"

static const char *TAG = "power";

// ---- low-battery protection ----
// Power off before the pack is deep-discharged. Thresholds apply to the load-compensated
// estimate (see load_offset()); 3.30 V matches li_ion_pct()'s 0%. A sustained run of
// samples is required so short sags (bass, SD bursts) do not trip it.
#define BATT_CUTOFF_V   3.30f
#define BATT_WARN_V     3.45f
#define BATT_CUTOFF_S   10      // consecutive 1 Hz samples under cutoff before acting
// Brownout guard on the raw rail (what the regulator sees); lower and faster than the
// cutoff above.
#define BATT_BROWNOUT_V 3.05f
#define BATT_BROWNOUT_S 3
// Grace period after power-up before the battery guard may shut anything down — see
// battery_guard(). Covers rail settling, inrush and the first ADC reads.
#define GUARD_GRACE_MS 8000u
// 1.85C V2 has no software power latch — "power off" just deep-sleeps and the
// BOOT button (GPIO0, active-low) wakes it.
#define PWR_GPIO EXAMPLE_PIN_NUM_BOOT_BTN

static power_btn_cb_t boot_cb;
static volatile bool   idle_off_en;
static volatile int    idle_timeout_s = 300;
// 32-bit so the cross-task read/write is a single atomic word on the S3 (a 64-bit value
// can tear across the low-word carry and spuriously blank/power-off). ms wraps every
// ~49.7 days, but the idle delta is computed with uint32_t modular subtraction, so the
// wrap is harmless.
static volatile uint32_t last_activity_ms;
static int             cur_brightness = 80;
static volatile bool   screen_off;
static volatile int    screen_timeout_s = 30;   // auto screen-off; 0 = never
static SemaphoreHandle_t screen_mux;            // serialises screen_set_off
static display_sleep_cb_t disp_sleep_cb;
static display_powerdown_cb_t disp_pd_cb;
// Speaker amp is on when audio plays OR the screen is on (for pop-free UI clicks);
// it drops only once the screen is off AND nothing is playing.
static volatile bool   amp_play;
static volatile bool   amp_screen = true;
static volatile bool   batt_low;         // under BATT_WARN_V; for the UI's warning

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void apply_backlight(int pct) { setUpduty(pct * 255 / 100); }  // active-high on 1.85C V2

// NS4150B speaker amp enable on GPIO15 (active-high). On the 1.85C V2 this is a
// plain GPIO, not an expander pin.
static void amp_apply(void)
{
    gpio_set_level(EXAMPLE_PIN_NUM_AMP_EN, (amp_play || amp_screen) ? 1 : 0);
}

// idle_task (prio 2) and button_task (prio 4, via app_power_wake) both call this on
// different cores, so the check-then-act on screen_off must be atomic — otherwise
// idle_task can blank the panel just as button_task finishes waking it.
static void screen_set_off(bool off)
{
    if (screen_mux) xSemaphoreTake(screen_mux, portMAX_DELAY);
    if (off != screen_off) {
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
    if (screen_mux) xSemaphoreGive(screen_mux);
}

void app_power_set_display_sleep_cb(display_sleep_cb_t cb) { disp_sleep_cb = cb; }

void app_power_set_display_powerdown_cb(display_powerdown_cb_t cb) { disp_pd_cb = cb; }

void app_power_set_amp(bool on) { amp_play = on; amp_apply(); }

void app_power_user_activity(void)
{
    last_activity_ms = (uint32_t)now_ms();   // keep the screen awake; do NOT wake it from off
}

bool app_power_screen_is_off(void) { return screen_off; }

bool app_power_battery_low(void) { return batt_low; }

void app_power_wake(void)
{
    last_activity_ms = (uint32_t)now_ms();
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

// ---- state of charge ----
//
// li_ion_pct() maps OPEN-CIRCUIT voltage, but the ADC reads TERMINAL voltage, which is
// OCV minus I x R_internal: it sags under load, and sits near the charger's ~4.2 V
// setpoint while charging.
//
// No current sense on this board, so a per-load-state offset approximates I x R. The CAL
// log line prints raw/compensated/percent for tuning.
#define IR_PLAYING  0.060f      // decode + speaker amp + SD reads
#define IR_SCREEN   0.015f      // panel + backlight, not playing
// Charging is inferred (no charge-status GPIO): a level above CHG_V_HARD sets it
// outright; otherwise the voltage trend decides, and a drop off the peak clears it.
#define CHG_V_HARD  4.18f       // unreachable on battery alone => definitely on charge
#define CHG_RISE    0.012f      // smoothed climb across a window => charging
#define CHG_FALL    0.012f      // smoothed fall across a window => not charging
#define CHG_DROP    0.030f      // fall below the charging peak => unplugged, release now
#define CHG_WIN_MS  30000u
// Maximum time the charging state may hold the gauge above the measured level.
#define CHG_FREEZE_MAX_MS 600000u   // 10 minutes

static volatile bool batt_charging;

static float load_offset(void)
{
    if (amp_play)    return IR_PLAYING;
    if (!screen_off) return IR_SCREEN;
    return 0.0f;
}

int app_power_battery_pct(void)
{
    static float ema = -1.0f;        // load-COMPENSATED, for the state-of-charge curve
    static float rema = -1.0f;       // RAW rail, for charge detection — see below
    static float ref = -1.0f;        // slow reference for the trend test
    static float chg_peak = -1.0f;   // highest smoothed RAW reading seen while charging
    static uint32_t ref_ms, cal_ms;
    static uint32_t froze_ms;        // when the gauge started being held above `target`
    static int shown = -1;

    float raw = app_power_battery_volts();
    if (raw <= 0.5f) return shown < 0 ? 0 : shown;

    float ocv = raw + load_offset();
    ema = (ema < 0) ? ocv : (ema * 0.92f + ocv * 0.08f);
    // Charge detection uses the RAW rail: the compensated estimate jumps by IR_PLAYING
    // when playback starts, which the trend test would read as a charger being plugged in.
    rema = (rema < 0) ? raw : (rema * 0.92f + raw * 0.08f);

    uint32_t now = (uint32_t)now_ms();
    if (ref < 0) { ref = rema; ref_ms = now; }

    if (raw > CHG_V_HARD) batt_charging = true;      // only a charger reaches this
    if ((uint32_t)(now - ref_ms) > CHG_WIN_MS) {
        float d = rema - ref;
        ref = rema; ref_ms = now;
        if (d > CHG_RISE)       batt_charging = true;
        else if (d < -CHG_FALL) batt_charging = false;
        // A plateau holds the current state: a full pack ON the charger is flat, and so
        // is an idle one off it — the trend cannot separate those, so don't guess.
    }
    // Fast release. Pulling the cable drops the terminal voltage off the charger's
    // setpoint within seconds, so watch for a fall below the peak seen while charging
    // rather than waiting a full trend window.
    if (batt_charging) {
        if (chg_peak < 0 || rema > chg_peak) chg_peak = rema;
        else if (rema < chg_peak - CHG_DROP) { batt_charging = false; chg_peak = -1.0f; }
    } else {
        chg_peak = -1.0f;
    }

    int target = li_ion_pct(ema);
    // The gauge moves one way only (down when discharging, up when charging). The charging
    // hold is bounded by CHG_FREEZE_MAX_MS so a wrong flag cannot hide a draining pack.
    if (shown < 0) { shown = target; froze_ms = now; }          // snap on the first read
    else if (batt_charging && target < shown) {
        if ((uint32_t)(now - froze_ms) > CHG_FREEZE_MAX_MS) {
            shown--;
            ESP_LOGW(TAG, "gauge held above %d%% while 'charging' — releasing", target);
        }
    } else {
        froze_ms = now;                                          // not holding anything back
        if (batt_charging)     { if (target > shown) shown++; }
        else if (target < shown) shown--;
    }

    if ((uint32_t)(now - cal_ms) > 30000u) {
        cal_ms = now;
        ESP_LOGI(TAG, "CAL batt raw=%.3f ocv=%.3f ema=%.3f pct=%d%s",
                 raw, ocv, ema, shown, batt_charging ? " CHG" : "");
    }
    return shown;
}

bool app_power_is_charging(void) { return batt_charging; }

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

    // Save the exact resume position before sleeping — ui_tick only persists every ~5 s,
    // so without this a power-off (or dying battery) loses up to 5 s of position.
    player_status_t st;
    app_player_get_status(&st);
    if (st.book_idx >= 0) app_store_set_last(st.book_idx, st.chapter, st.pos_ms);

    // Stop the player and close the codec device. A plain stop leaves the ES8311 open,
    // and its analog section then draws several mA for the whole "off" period — this
    // board has no load switch, so deep sleep cannot cut it any other way.
    app_player_suspend();

    amp_play = false; amp_screen = false; amp_apply();   // silence the amp
    setUpduty(0);                                         // backlight off

    // Sleep In the panel controller, not just DISPOFF. DISPOFF alone blanks the output
    // but leaves the ST77916 oscillator and booster running — again, milliamps that
    // survive deep sleep. Falls back to a plain blank if no power-down hook is wired.
    if (disp_pd_cb) disp_pd_cb();
    else if (disp_sleep_cb) disp_sleep_cb(true);

    // Wait for BOOT to be released before arming EXT1 — a long-press held past sleep
    // entry keeps GPIO0 low and would immediately wake the device back up.
    //
    // Bounded wait: a worn BOOT switch that reads low forever must not strand the device
    // awake. If it never releases, sleep anyway (EXT1 re-wakes and the log shows it).
    int waited_ms = 0;
    while (gpio_get_level(PWR_GPIO) == 0 && waited_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    if (gpio_get_level(PWR_GPIO) == 0)
        ESP_LOGE(TAG, "BOOT/GPIO0 still low after %d ms — sleeping anyway; expect an "
                      "immediate EXT1 re-wake (check the switch)", waited_ms);
    vTaskDelay(pdMS_TO_TICKS(50));   // debounce the release

    // Last line that reaches the SD card. If LOG.TXT ends here, deep sleep was entered;
    // if it ends earlier, power-off stalled before this point and the device stayed awake.
    ESP_LOGW(TAG, "entering deep sleep (batt %.3f V)", app_power_battery_volts());

    // Quiesce SD/log I/O before cutting power: flush the log and hold the flush lock,
    // then unmount the FAT volume so a power-off can't land mid FAT update.
    // Compact progress files before the log is pinned and the volume unmounted.
    app_played_flush();
    app_log_suspend();
    sdcard_unmount();

    // Deep-sleep pin hygiene: digital pull-ups power down in deep sleep, so
    // latch the amp (GPIO15) and backlight (GPIO5) enables low — floating, they can
    // partially enable the amp / BL driver and drain the battery while "off" — and hold
    // the RTC pull-up on BOOT/GPIO0 so the aging switch line can't float low and either
    // spuriously wake the device or block sleep.
    gpio_set_level(EXAMPLE_PIN_NUM_AMP_EN, 0);
    gpio_hold_en(EXAMPLE_PIN_NUM_AMP_EN);
    gpio_reset_pin(EXAMPLE_PIN_NUM_BK_LIGHT);            // detach LEDC -> plain GPIO
    gpio_set_direction(EXAMPLE_PIN_NUM_BK_LIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, 0);
    gpio_hold_en(EXAMPLE_PIN_NUM_BK_LIGHT);
    gpio_deep_sleep_hold_en();
    rtc_gpio_pullup_en(GPIO_NUM_0);
    rtc_gpio_pulldown_dis(GPIO_NUM_0);

    // No hardware latch on this board: deep-sleep and wake on BOOT (active low).
    esp_sleep_enable_ext1_wakeup(1ULL << PWR_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void app_power_set_idle_off(bool enable, int timeout_s)
{
    idle_off_en = enable;
    if (timeout_s > 0) idle_timeout_s = timeout_s;
    last_activity_ms = (uint32_t)now_ms();
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
                last_activity_ms = (uint32_t)now_ms();
                if (boot_cb) boot_cb();            // already on -> go Home
            }
        }
    }
}

// Watch the pack and shut down cleanly before the regulator browns out.
//
// Returns true if it powered off (caller must stop touching hardware).
static bool battery_guard(void)
{
    static int low_run, crit_run;

    // Power-up inrush and rail settling produce low readings that say nothing about the
    // cell; ignore the first GUARD_GRACE_MS.
    if ((uint32_t)now_ms() < GUARD_GRACE_MS) { low_run = crit_run = 0; return false; }

    float raw = app_power_battery_volts();
    if (raw <= 0.5f) { low_run = crit_run = 0; return false; }   // bad ADC read; ignore it

    // Log the minimum rail voltage seen while playing, to diagnose sudden power loss.
    static float    play_min = 99.0f;
    static uint32_t play_log_ms;
    player_status_t ps;
    app_player_get_status(&ps);
    if (ps.state == PLAYER_PLAYING) {
        if (raw < play_min) play_min = raw;
        uint32_t nw = (uint32_t)now_ms();
        if ((uint32_t)(nw - play_log_ms) > 5000u) {
            play_log_ms = nw;
            ESP_LOGI(TAG, "PLAY batt raw=%.3f min5=%.3f vol=%d", raw, play_min, ps.volume);
            play_min = 99.0f;
        }
    } else {
        play_min = 99.0f;
    }

    // Never act while a charger is holding the rail: the reading then belongs to the
    // charger, not the cell.
    if (app_power_is_charging()) { low_run = crit_run = 0; batt_low = false; return false; }

    float ocv = raw + load_offset();
    batt_low = (ocv < BATT_WARN_V);

    // app_power_off() saves the resume position, closes the codec, sleeps the panel and
    // unmounts the card, so either path below is a clean stop, not a collapse mid-write.
    if (raw < BATT_BROWNOUT_V) {                    // rail collapsing: act fast
        if (++crit_run >= BATT_BROWNOUT_S) {
            ESP_LOGE(TAG, "rail collapsing (%.3f V) — emergency power off", raw);
            app_power_off();
            return true;
        }
    } else {
        crit_run = 0;
    }

    if (ocv >= BATT_CUTOFF_V) { low_run = 0; return false; }
    if (++low_run < BATT_CUTOFF_S) {
        ESP_LOGW(TAG, "battery low: raw=%.3f ocv=%.3f (%d/%d)", raw, ocv, low_run, BATT_CUTOFF_S);
        return false;
    }
    ESP_LOGE(TAG, "battery empty (raw=%.3f ocv=%.3f) — powering off to protect the cell", raw, ocv);
    app_power_off();
    return true;
}

static void idle_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (battery_guard()) continue;                           // powered off; nothing else to do
        uint32_t idle = (uint32_t)now_ms() - last_activity_ms;   // wrap-safe modular delta

        // Screen auto-off after touch inactivity — even while audio plays.
        if (screen_timeout_s > 0 && !screen_off && idle > (uint32_t)screen_timeout_s * 1000)
            screen_set_off(true);

        // Device power-off only when idle AND nothing is playing.
        if (idle_off_en) {
            player_status_t st;
            app_player_get_status(&st);
            if (st.state != PLAYER_PLAYING && idle > (uint32_t)idle_timeout_s * 1000) {
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
    // Release any pin holds latched by a previous deep-sleep power-off. Deep sleep holds AMP_EN
    // (GPIO15) and the backlight (GPIO5) low before sleeping, and those holds SURVIVE the
    // EXT1 wake reset — so without releasing them here the backlight and amp stay stuck
    // off after waking and the device boots to a black, silent screen that looks dead.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(EXAMPLE_PIN_NUM_AMP_EN);
    gpio_hold_dis(EXAMPLE_PIN_NUM_BK_LIGHT);
    rtc_gpio_hold_dis(GPIO_NUM_0);          // return BOOT/GPIO0 from RTC to normal GPIO use

    screen_mux = xSemaphoreCreateMutex();
    amp_setup();
    adc_bsp_init();
    button_Init();
    lcd_bl_pwm_bsp_init(0);        // start dark (active-high duty); real brightness set next
    app_power_set_brightness(app_store_get_brightness(80));
    last_activity_ms = (uint32_t)now_ms();
    xTaskCreatePinnedToCore(button_task, "btn", 3 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(idle_task, "idle", 3 * 1024, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "power ready (batt %d%%, %.3f V)", app_power_battery_pct(), app_power_battery_volts());
}
