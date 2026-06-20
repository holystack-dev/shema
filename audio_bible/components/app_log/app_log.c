// app_log — persistent ESP_LOG capture to the SD card. See app_log.h.
//
// Design (and why): the esp_log vprintf hook only appends to a fixed PSRAM ring
// buffer — no I/O — so it never blocks the logging task and the buffer can
// never grow. A low-priority task on core 0 (away from the player on core 1)
// drains the ring to /sdcard/LOG.TXT every few seconds. SD writes, unlike
// internal-flash writes, do NOT disable the instruction cache, so playback is
// undisturbed (audio: no compromise). Overflow is bounded twice: the RAM ring
// overwrites its oldest bytes, and the SD file rotates to LOG.OLD past a cap.
#include "app_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define LOG_PATH      "/sdcard/LOG.TXT"
#define LOG_PATH_OLD  "/sdcard/LOG.OLD"
#define RING_SZ       (16 * 1024)     // RAM ring; overwrites oldest on overflow
#define FILE_CAP      (128 * 1024)    // rotate LOG.TXT -> LOG.OLD past this
#define FLUSH_MS      10000           // background flush cadence

static char             *ring;        // RING_SZ bytes (PSRAM)
static char             *snap;        // RING_SZ scratch for the flush copy
static size_t            r_head;      // index of the next byte to write
static size_t            r_count;     // bytes pending (clamped to RING_SZ)
static SemaphoreHandle_t r_mux;
static vprintf_like_t    prev_vprintf;
static volatile bool     in_flush;    // suppress capture of our own SD I/O logs
static long              file_bytes = -1;

// Append n bytes to the ring, dropping the oldest on overflow. Holds r_mux.
static void ring_put(const char *s, int n)
{
    if (n <= 0) return;
    if (n > RING_SZ) { s += (n - RING_SZ); n = RING_SZ; }   // keep only the tail
    size_t end = RING_SZ - r_head;
    if ((size_t)n <= end) {
        memcpy(ring + r_head, s, n);
    } else {
        memcpy(ring + r_head, s, end);
        memcpy(ring, s + end, n - end);
    }
    r_head = (r_head + n) % RING_SZ;
    r_count += n;
    if (r_count > RING_SZ) r_count = RING_SZ;               // oldest overwritten
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int ret = prev_vprintf ? prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (ring && !in_flush) {
        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;  // truncated line: fine
        // Non-blocking take: a logger must never stall on the flush copy; at
        // worst we drop one captured line during the brief drain.
        if (n > 0 && xSemaphoreTake(r_mux, 0) == pdTRUE) {
            ring_put(buf, n);
            xSemaphoreGive(r_mux);
        }
    }
    va_end(ap2);
    return ret;
}

void app_log_flush(void)
{
    if (!ring || !snap || !r_mux) return;

    xSemaphoreTake(r_mux, portMAX_DELAY);
    size_t n = r_count;
    if (n) {
        size_t start = (r_head + RING_SZ - n) % RING_SZ;    // oldest pending byte
        size_t first = RING_SZ - start;
        if (first >= n) {
            memcpy(snap, ring + start, n);
        } else {
            memcpy(snap, ring + start, first);
            memcpy(snap + first, ring, n - first);
        }
        r_count = 0;
    }
    xSemaphoreGive(r_mux);
    if (!n) return;

    in_flush = true;
    FILE *f = fopen(LOG_PATH, "a");
    if (f) {
        fwrite(snap, 1, n, f);
        file_bytes = ftell(f);
        fclose(f);
        if (file_bytes > FILE_CAP) {           // rotate; keep one old generation
            remove(LOG_PATH_OLD);
            rename(LOG_PATH, LOG_PATH_OLD);
            file_bytes = 0;
        }
    }
    in_flush = false;
}

static void flush_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));
        app_log_flush();
    }
}

esp_err_t app_log_init(void)
{
    if (ring) return ESP_OK;
    ring = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM);
    snap = heap_caps_malloc(RING_SZ, MALLOC_CAP_SPIRAM);
    if (!ring || !snap) {                      // PSRAM unavailable: don't burn DRAM
        free(ring); free(snap); ring = snap = NULL;
        return ESP_ERR_NO_MEM;
    }
    r_mux = xSemaphoreCreateMutex();
    if (!r_mux) { free(ring); free(snap); ring = snap = NULL; return ESP_ERR_NO_MEM; }
    r_head = r_count = 0;

    // A boot marker makes power-cycles easy to find when reading the file back.
    in_flush = true;
    FILE *f = fopen(LOG_PATH, "a");
    if (f) { fputs("\n==== boot ====\n", f); file_bytes = ftell(f); fclose(f); }
    in_flush = false;

    prev_vprintf = esp_log_set_vprintf(log_vprintf);
    xTaskCreatePinnedToCore(flush_task, "logflush", 4 * 1024, NULL, 2, NULL, 0);
    ESP_LOGI("app_log", "persistent log -> %s (ring %dB, file cap %dB)",
             LOG_PATH, RING_SZ, FILE_CAP);
    return ESP_OK;
}
