// Included only in BIBLE_SCREENSHOTS builds. Actual display frames are copied
// into a bounded PSRAM queue; USB compression/output never holds the LVGL lock.
// Runs are encoded as big-endian uint16 count + the original two RGB565 bytes,
// then deflated by the chip's ROM miniz to keep USB bandwidth below render rate.
#include <stdlib.h>
#include "miniz.h"
#define VIDEO_SLOTS 8
#define VIDEO_PIXELS (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES)
static uint8_t *video_frames[VIDEO_SLOTS], *video_encoded, *video_deflated;
static tdefl_compressor *video_compressor;
static uint32_t video_times[VIDEO_SLOTS], video_start_ms, video_count, video_dropped;
static QueueHandle_t video_free, video_ready;
static SemaphoreHandle_t video_done;
static bool video_enabled;

static void video_writer(void *arg)
{
    (void)arg;
    for (;;) {
        int slot;
        xQueueReceive(video_ready, &slot, portMAX_DELAY);
        if (slot < 0) { xSemaphoreGive(video_done); continue; }
        const uint8_t *raw = video_frames[slot];
        size_t n = 0;
        for (size_t i = 0; i < VIDEO_PIXELS;) {
            unsigned run = 1;
            while (i + run < VIDEO_PIXELS && run < 65535 &&
                   raw[2 * i] == raw[2 * (i + run)] && raw[2 * i + 1] == raw[2 * (i + run) + 1]) run++;
            video_encoded[n++] = run >> 8;
            video_encoded[n++] = run;
            video_encoded[n++] = raw[2 * i];
            video_encoded[n++] = raw[2 * i + 1];
            i += run;
        }
        size_t input_size = n, compressed_size = VIDEO_PIXELS * 4 + 4096;
        tdefl_init(video_compressor, NULL, NULL, TDEFL_WRITE_ZLIB_HEADER | 32);
        tdefl_status status = tdefl_compress(video_compressor, video_encoded, &input_size,
                                           video_deflated, &compressed_size, TDEFL_FINISH);
        if (status != TDEFL_STATUS_DONE || input_size != n) {
            ESP_LOGE(TAG, "Video compression failed");
            xQueueSend(video_free, &slot, portMAX_DELAY);
            continue;
        }
        n = compressed_size;
        printf("\n<<<FRAME %lu %lu %u>>>\n", (unsigned long)video_count,
               (unsigned long)video_times[slot], (unsigned)n);
        char line[1030];
        for (size_t i = 0; i < n;) {
            int out = 0;
            line[out++] = 'V'; line[out++] = ':';
            for (int g = 0; g < 256 && i < n; g++) {
                uint32_t v = 0; int have = 0;
                for (int k = 0; k < 3; k++) { v <<= 8; if (i < n) { v |= video_deflated[i++]; have++; } }
                line[out++] = SHOT_B64[(v >> 18) & 63];
                line[out++] = SHOT_B64[(v >> 12) & 63];
                line[out++] = have > 1 ? SHOT_B64[(v >> 6) & 63] : '=';
                line[out++] = have > 2 ? SHOT_B64[v & 63] : '=';
            }
            line[out++] = '\n';
            fwrite(line, 1, out, stdout);
        }
        printf("<<<ENDFRAME %lu>>>\n", (unsigned long)video_count++);
        fflush(stdout);
        xQueueSend(video_free, &slot, portMAX_DELAY);
        vTaskDelay(1);
    }
}

static bool video_init(void)
{
    if (video_ready) return true;
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        video_frames[i] = heap_caps_malloc(VIDEO_PIXELS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!video_frames[i]) goto failed;
    }
    video_encoded = heap_caps_malloc(VIDEO_PIXELS * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    video_deflated = heap_caps_malloc(VIDEO_PIXELS * 4 + 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    video_compressor = heap_caps_malloc(sizeof *video_compressor, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!video_encoded || !video_deflated || !video_compressor) goto failed;
    video_free = xQueueCreate(VIDEO_SLOTS, sizeof(int));
    video_ready = xQueueCreate(VIDEO_SLOTS, sizeof(int));
    video_done = xSemaphoreCreateBinary();
    if (!video_free || !video_ready || !video_done) goto failed;
    // ROM deflate has a much larger stack requirement than the RLE/USB loop.
    if (xTaskCreatePinnedToCore(video_writer, "video", 32768, NULL, 2, NULL, 0) != pdPASS) goto failed;
    for (int i = 0; i < VIDEO_SLOTS; i++) xQueueSend(video_free, &i, 0);
    return true;
failed:
    for (int i = 0; i < VIDEO_SLOTS; i++) { free(video_frames[i]); video_frames[i] = NULL; }
    free(video_encoded); video_encoded = NULL;
    free(video_deflated); video_deflated = NULL;
    free(video_compressor); video_compressor = NULL;
    if (video_free) vQueueDelete(video_free);
    if (video_ready) vQueueDelete(video_ready);
    if (video_done) vSemaphoreDelete(video_done);
    video_free = video_ready = NULL; video_done = NULL;
    return false;
}

static void video_submit(const uint8_t *raw)
{
    if (!video_enabled) return; // called under the LVGL lock, after the real flush
    int slot;
    if (xQueueReceive(video_free, &slot, 0) != pdTRUE) { video_dropped++; return; }
    memcpy(video_frames[slot], raw, VIDEO_PIXELS * 2);
    video_times[slot] = lv_tick_get() - video_start_ms;
    xQueueSend(video_ready, &slot, 0);
}

static void video_command(bool start)
{
    if (start) {
        if (video_enabled || !video_init()) { printf("<<<VIDEO ERROR>>>\n"); fflush(stdout); return; }
        bible_lvgl_lock(-1);
        video_count = video_dropped = 0;
        video_start_ms = lv_tick_get();
        video_enabled = true;
        lv_obj_invalidate(lv_scr_act());
        printf("<<<VIDEO START 360 360>>>\n"); fflush(stdout);
        bible_lvgl_unlock();
    } else {
        bible_lvgl_lock(-1);
        bool active = video_enabled;
        video_enabled = false;
        uint32_t elapsed = lv_tick_get() - video_start_ms;
        bible_lvgl_unlock();
        if (!active) { printf("<<<VIDEO ERROR>>>\n"); fflush(stdout); return; }
        int stop = -1;
        xQueueSend(video_ready, &stop, portMAX_DELAY);
        xSemaphoreTake(video_done, portMAX_DELAY);
        printf("<<<VIDEO STOP %lu %lu %lu>>>\n", (unsigned long)video_count,
               (unsigned long)video_dropped, (unsigned long)elapsed);
        fflush(stdout);
    }
}
