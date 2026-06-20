// app_player — Helix MP3 decode -> esp_codec_dev (ES8311 via codec_board).
#include "app_player.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_codec_dev.h"
#include "codec_board.h"
#include "codec_init.h"
#include "mp3dec.h"
#include "bible_data.h"

static const char *TAG = "player";

#define SD_MOUNT       "/sdcard"
#define INBUF_SZ       (4 * 1024)
#define MAX_PCM_SAMP   (MAX_NGRAN * MAX_NSAMP * MAX_NCHAN)   // 2304 shorts
#define MAX_MONO_SAMP  (MAX_NGRAN * MAX_NSAMP)               // 1152 samples/frame

typedef enum { CMD_PLAY, CMD_PAUSE_TOGGLE, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_SEEK, CMD_CLICK } cmd_type_t;

#define CLICK_SAMP 480   // 10 ms @ 48 kHz
typedef struct {
    cmd_type_t type;
    int        book_idx;
    int        chapter;
    uint32_t   arg;     // seek target ms
} player_cmd_t;

// ---- shared state (guarded by st_mux) ----
static SemaphoreHandle_t st_mux;
static player_status_t   g_st = { .state = PLAYER_STOPPED, .book_idx = -1, .volume = 70 };

static QueueHandle_t      cmd_q;
static esp_codec_dev_handle_t playback;
static HMP3Decoder        mp3;
static player_event_cb_t  evt_cb;
static player_amp_cb_t    amp_cb;

static void amp(bool on) { if (amp_cb) amp_cb(on); }

// ---- player-task-local ----
static FILE     *fp;
static long      track_size;
static uint8_t   in_buf[INBUF_SZ];
static uint8_t  *in_ptr;
static int       in_left;
static short     pcm[MAX_PCM_SAMP];
static short     pcm_stereo[MAX_MONO_SAMP * 2];
static int       cur_sr, cur_ch;          // currently opened codec format
static uint64_t  pos_samples;             // decoded samples (per channel)
static int       name_fmt = -1;           // 0: "%d_%d", 1: "%02d_%02d"
static short     click_pcm[CLICK_SAMP * 2];  // pre-rendered UI tick (48k stereo)

void app_player_get_status(player_status_t *out)
{
    xSemaphoreTake(st_mux, portMAX_DELAY);
    *out = g_st;
    xSemaphoreGive(st_mux);
}

// ---- audio source: BIBLE/<LANG>/<VERSION>/<book>_<chapter>.mp3 on the SD card ----
#define VER_ROOT   SD_MOUNT "/BIBLE"
#define MAX_VERS   12
typedef struct { char dir[96]; char name[40]; char shortn[32]; } ver_t;
static ver_t g_vers[MAX_VERS];
static int   g_vers_n = -1;                  // -1 = not scanned yet
static char  g_base[96] = SD_MOUNT "/AUDIO"; // legacy default until a version is chosen

// File naming under the base, auto-detected: "1_1.mp3" vs "01_01.mp3".
static const char *const NAME_FMT[] = { "%s/%d_%d.mp3", "%s/%02d_%02d.mp3" };

static FILE *open_chapter(int book_id, int chapter)
{
    char path[128];
    if (name_fmt >= 0) {
        snprintf(path, sizeof(path), NAME_FMT[name_fmt], g_base, book_id, chapter);
        return fopen(path, "rb");
    }
    for (int f = 0; f < (int)(sizeof(NAME_FMT) / sizeof(NAME_FMT[0])); f++) {
        snprintf(path, sizeof(path), NAME_FMT[f], g_base, book_id, chapter);
        FILE *t = fopen(path, "rb");
        if (t) { name_fmt = f; ESP_LOGI(TAG, "audio base %s (fmt #%d)", g_base, f); return t; }
    }
    return NULL;
}

static bool is_dir(const char *p) { DIR *d = opendir(p); if (d) { closedir(d); return true; } return false; }

// Scan BIBLE/<LANG>/<VERSION> once; cache the list.
int app_player_versions(void)
{
    if (g_vers_n >= 0) return g_vers_n;
    g_vers_n = 0;
    DIR *ld = opendir(VER_ROOT);
    if (!ld) { ESP_LOGW(TAG, "no %s on card", VER_ROOT); return 0; }
    struct dirent *le;
    while ((le = readdir(ld)) != NULL && g_vers_n < MAX_VERS) {
        if (le->d_name[0] == '.') continue;
        char lpath[96]; snprintf(lpath, sizeof lpath, "%s/%.28s", VER_ROOT, le->d_name);
        if (!is_dir(lpath)) continue;
        DIR *vd = opendir(lpath);
        if (!vd) continue;
        struct dirent *ve;
        while ((ve = readdir(vd)) != NULL && g_vers_n < MAX_VERS) {
            if (ve->d_name[0] == '.') continue;
            char vpath[96]; snprintf(vpath, sizeof vpath, "%.60s/%.28s", lpath, ve->d_name);
            if (!is_dir(vpath)) continue;
            ver_t *v = &g_vers[g_vers_n++];
            snprintf(v->dir, sizeof v->dir, "%.95s", vpath);
            char pretty[32]; int j = 0;                    // "POC_Reading" -> "POC Reading"
            for (int i = 0; ve->d_name[i] && j < (int)sizeof(pretty) - 1; i++)
                pretty[j++] = (ve->d_name[i] == '_') ? ' ' : ve->d_name[i];
            pretty[j] = 0;
            snprintf(v->name, sizeof v->name, "%.8s  %.28s", le->d_name, pretty);  // "ML  POC Dramatized"
            snprintf(v->shortn, sizeof v->shortn, "%.31s", pretty);                // "POC Dramatized"
        }
        closedir(vd);
    }
    closedir(ld);
    ESP_LOGI(TAG, "found %d audio version(s) under %s", g_vers_n, VER_ROOT);
    return g_vers_n;
}
const char *app_player_version_name(int i) { return (i >= 0 && i < g_vers_n) ? g_vers[i].name : ""; }
const char *app_player_version_short(int i){ return (i >= 0 && i < g_vers_n) ? g_vers[i].shortn : ""; }
const char *app_player_version_dir(int i)  { return (i >= 0 && i < g_vers_n) ? g_vers[i].dir  : ""; }
int app_player_find_version(const char *dir)
{
    for (int i = 0; i < g_vers_n; i++) if (strcmp(g_vers[i].dir, dir) == 0) return i;
    return -1;
}
void app_player_set_base(const char *dir)
{
    if (!dir || !dir[0]) return;
    snprintf(g_base, sizeof g_base, "%s", dir);
    name_fmt = -1;   // re-detect filename format under the new base
}

static int refill(void)
{
    if (in_left > 0 && in_ptr != in_buf) memmove(in_buf, in_ptr, in_left);
    in_ptr = in_buf;
    int got = fread(in_buf + in_left, 1, INBUF_SZ - in_left, fp);
    if (got > 0) in_left += got;
    return got;
}

static void close_track(void)
{
    if (fp) { fclose(fp); fp = NULL; }
    in_ptr = in_buf;
    in_left = 0;
}

static bool start_track(int book_idx, int chapter)
{
    close_track();
    if (book_idx < 0 || book_idx >= BIBLE_BOOK_COUNT) return false;
    int cc = BIBLE_BOOKS[book_idx].chapter_count;
    if (chapter < 1 || chapter > cc) return false;

    fp = open_chapter(BIBLE_BOOKS[book_idx].id, chapter);
    if (!fp) {
        ESP_LOGE(TAG, "missing audio: book %d (%s) ch %d",
                 BIBLE_BOOKS[book_idx].id, BIBLE_BOOKS[book_idx].name, chapter);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    track_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    pos_samples = 0;

    // Duration straight from the file (CBR bitrate x audio size). Works for every
    // version/recording, unlike a table baked from one source. Skip any ID3v2 tag,
    // read the first frame header for the bitrate, then rewind for the decode loop.
    uint32_t dur = 0;
    {
        unsigned char id3[10];
        long start = 0;
        if (fread(id3, 1, 10, fp) == 10 && id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3')
            start = 10 + (((long)(id3[6] & 0x7f) << 21) | ((id3[7] & 0x7f) << 14) |
                          ((id3[8] & 0x7f) << 7) | (id3[9] & 0x7f));
        fseek(fp, start, SEEK_SET);
        int n = fread(in_buf, 1, INBUF_SZ, fp);
        int off = (n > 0) ? MP3FindSyncWord(in_buf, n) : -1;
        MP3FrameInfo fi;
        if (off >= 0 && MP3GetNextFrameInfo(mp3, &fi, in_buf + off) == ERR_MP3_NONE && fi.bitrate > 0)
            dur = (uint32_t)((uint64_t)(track_size - start) * 8000ULL / (uint32_t)fi.bitrate);
        fseek(fp, 0, SEEK_SET);
        in_ptr = in_buf; in_left = 0;
    }
    int vol;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    g_st.state = PLAYER_PLAYING; g_st.book_idx = book_idx; g_st.chapter = chapter;
    g_st.pos_ms = 0; g_st.dur_ms = dur; vol = g_st.volume;
    xSemaphoreGive(st_mux);

    ESP_LOGI(TAG, "play %s %d (%ld bytes, %lu ms) vol=%d",
             BIBLE_BOOKS[book_idx].name, chapter, track_size, dur, vol);
    amp(true);   // enable amp before any PCM is written (no clipped start)
    return true;
}

static void ensure_codec(int sr, int ch)
{
    if (sr == cur_sr && ch == cur_ch) return;
    if (cur_sr) esp_codec_dev_close(playback);
    esp_codec_dev_sample_info_t fs = { .sample_rate = sr, .channel = 2, .bits_per_sample = 16 };
    int rc = esp_codec_dev_open(playback, &fs);
    if (rc != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "esp_codec_dev_open failed (%d)", rc); }
    else ESP_LOGI(TAG, "Open codec device OK %d Hz", sr);
    int vol;
    xSemaphoreTake(st_mux, portMAX_DELAY); vol = g_st.volume; xSemaphoreGive(st_mux);
    esp_codec_dev_set_out_vol(playback, (float)vol);
    cur_sr = sr; cur_ch = ch;
}

// returns: 0 decoded a frame, 1 EOF, -1 fatal
static int decode_frame(void)
{
    if (in_left < MAINBUF_SIZE) {
        if (refill() == 0 && in_left == 0) return 1; // EOF, nothing buffered
    }
    int off = MP3FindSyncWord(in_ptr, in_left);
    if (off < 0) {
        in_left = 0;                        // no sync in buffer; drop & refill
        return (refill() == 0) ? 1 : 0;
    }
    in_ptr += off; in_left -= off;
    if (in_left < MAINBUF_SIZE) refill();

    int err = MP3Decode(mp3, &in_ptr, &in_left, pcm, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
        return (refill() == 0 && in_left == 0) ? 1 : 0;
    }
    if (err != ERR_MP3_NONE) {
        ESP_LOGW(TAG, "MP3 decode error: %d", err);
        if (in_left > 0) { in_ptr++; in_left--; }   // skip a byte, resync
        return 0;
    }

    MP3FrameInfo fi;
    MP3GetLastFrameInfo(mp3, &fi);
    if (fi.outputSamps <= 0) return 0;
    ensure_codec(fi.samprate, fi.nChans);

    int nsamp = (fi.nChans == 1) ? fi.outputSamps : fi.outputSamps / 2;
    short *out = pcm;
    int out_bytes;
    if (fi.nChans == 1) {                    // expand mono -> stereo for the codec
        for (int i = 0; i < nsamp; i++) { pcm_stereo[2 * i] = pcm_stereo[2 * i + 1] = pcm[i]; }
        out = pcm_stereo;
        out_bytes = nsamp * 2 * sizeof(short);
    } else {
        out_bytes = fi.outputSamps * sizeof(short);
    }
    esp_codec_dev_write(playback, out, out_bytes);

    pos_samples += nsamp;
    uint32_t ms = (uint32_t)(pos_samples * 1000 / (fi.samprate ? fi.samprate : 1));
    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.pos_ms = ms; xSemaphoreGive(st_mux);

    static uint32_t hb;
    if (++hb % 200 == 0) ESP_LOGI(TAG, "playing pos=%lu/%lu ms (%dHz/%dch)",
                                  (unsigned long)ms, (unsigned long)g_st.dur_ms, fi.samprate, fi.nChans);
    return 0;
}

// Render a soft 10 ms "tick": 2 kHz tone with a fast exponential decay.
static void gen_click(void)
{
    for (int i = 0; i < CLICK_SAMP; i++) {
        float env = expf(-(float)i / (CLICK_SAMP * 0.22f));
        short s = (short)(5000.0f * env * sinf(2.0f * (float)M_PI * 2000.0f * i / 48000.0f));
        click_pcm[2 * i] = click_pcm[2 * i + 1] = s;
    }
}

// Play the UI tick. Skipped while a chapter is playing so the stream and the
// saved position are never disturbed. Runs on the player task (owns the codec).
static void play_click(void)
{
    player_state_t state;
    xSemaphoreTake(st_mux, portMAX_DELAY); state = g_st.state; xSemaphoreGive(st_mux);
    if (state == PLAYER_PLAYING) return;
    ensure_codec(48000, 2);
    esp_codec_dev_write(playback, click_pcm, sizeof(click_pcm));
}

// compute next/prev (book_idx,chapter); returns false if past the end
static bool calc_step(int dir, int *bi, int *ch, bool repeat_all)
{
    int b = *bi, c = *ch + dir;
    if (dir > 0 && c > BIBLE_BOOKS[b].chapter_count) {
        b++; c = 1;
        if (b >= BIBLE_BOOK_COUNT) { if (!repeat_all) return false; b = 0; }
    } else if (dir < 0 && c < 1) {
        b--;
        if (b < 0) { if (!repeat_all) return false; b = BIBLE_BOOK_COUNT - 1; }
        c = BIBLE_BOOKS[b].chapter_count;
    }
    *bi = b; *ch = c;
    return true;
}

static void do_seek(uint32_t ms)
{
    if (!fp || track_size <= 0) return;
    uint32_t dur;
    int sr;
    xSemaphoreTake(st_mux, portMAX_DELAY); dur = g_st.dur_ms; xSemaphoreGive(st_mux);
    sr = cur_sr ? cur_sr : 44100;
    long pos = (dur > 0) ? (long)((double)ms / dur * track_size) : 0;
    if (pos < 0) pos = 0;
    if (pos >= track_size) pos = track_size - 1;
    fseek(fp, pos, SEEK_SET);
    in_ptr = in_buf; in_left = 0;
    pos_samples = (uint64_t)ms * sr / 1000;
    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.pos_ms = ms; xSemaphoreGive(st_mux);
}

static void handle_cmd(const player_cmd_t *c)
{
    switch (c->type) {
    case CMD_PLAY:
        start_track(c->book_idx, c->chapter);
        break;
    case CMD_STOP:
        close_track();
        xSemaphoreTake(st_mux, portMAX_DELAY); g_st.state = PLAYER_STOPPED; xSemaphoreGive(st_mux);
        amp(false);
        break;
    case CMD_PAUSE_TOGGLE: {
        bool now_playing;
        xSemaphoreTake(st_mux, portMAX_DELAY);
        if (g_st.state == PLAYER_PLAYING) g_st.state = PLAYER_PAUSED;
        else if (g_st.state == PLAYER_PAUSED && fp) g_st.state = PLAYER_PLAYING;
        now_playing = (g_st.state == PLAYER_PLAYING);
        xSemaphoreGive(st_mux);
        amp(now_playing);   // amp follows play/pause
        break;
    }
    case CMD_NEXT:
    case CMD_PREV: {
        int bi, ch; bool ra;
        xSemaphoreTake(st_mux, portMAX_DELAY); bi = g_st.book_idx; ch = g_st.chapter; ra = g_st.repeat_all; xSemaphoreGive(st_mux);
        if (bi < 0) break;
        if (calc_step(c->type == CMD_NEXT ? 1 : -1, &bi, &ch, ra)) start_track(bi, ch);
        break;
    }
    case CMD_SEEK:
        do_seek(c->arg);
        break;
    case CMD_CLICK:
        play_click();
        break;
    }
}

static void player_task(void *arg)
{
    player_cmd_t c;
    for (;;) {
        player_state_t state;
        xSemaphoreTake(st_mux, portMAX_DELAY); state = g_st.state; xSemaphoreGive(st_mux);

        if (state == PLAYER_PLAYING && fp) {
            // service any pending command without blocking
            if (xQueueReceive(cmd_q, &c, 0) == pdTRUE) { handle_cmd(&c); continue; }
            int r = decode_frame();
            if (r == 1) {                 // natural end of track -> auto next
                int bi, ch; bool ra;
                xSemaphoreTake(st_mux, portMAX_DELAY); bi = g_st.book_idx; ch = g_st.chapter; ra = g_st.repeat_all; xSemaphoreGive(st_mux);
                close_track();
                ESP_LOGI(TAG, "track end");
                if (calc_step(1, &bi, &ch, ra) && start_track(bi, ch)) {
                    // continued
                } else {
                    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.state = PLAYER_STOPPED; player_status_t s = g_st; xSemaphoreGive(st_mux);
                    amp(false);   // end of content -> drop amp
                    if (evt_cb) evt_cb(&s);
                }
            }
        } else {
            // paused or stopped: short poll so UI clicks stay responsive
            if (xQueueReceive(cmd_q, &c, pdMS_TO_TICKS(30)) == pdTRUE) handle_cmd(&c);
        }
    }
}

esp_err_t app_player_init(void)
{
    st_mux = xSemaphoreCreateMutex();
    cmd_q  = xQueueCreate(8, sizeof(player_cmd_t));

    set_codec_board_type("S3_LCD_1_85C");
    codec_init_cfg_t cfg = {
        .in_mode = CODEC_I2S_MODE_NONE,   // playback only; ES7210 ADC not used
        .out_mode = CODEC_I2S_MODE_TDM,
        .in_use_tdm = false,
        .reuse_dev = false,
    };
    int rc = init_codec(&cfg);
    if (rc != 0) { ESP_LOGE(TAG, "init_codec failed (%d)", rc); return ESP_FAIL; }
    playback = get_playback_handle();
    if (!playback) { ESP_LOGE(TAG, "no playback codec device"); return ESP_FAIL; }
    ESP_LOGI(TAG, "Audio HAL ready (ES8311 via codec_board)");

    mp3 = MP3InitDecoder();
    if (!mp3) { ESP_LOGE(TAG, "Failed to initialize MP3 decoder"); return ESP_FAIL; }
    ESP_LOGI(TAG, "MP3 decoder initialized");

    gen_click();

    xTaskCreatePinnedToCore(player_task, "player", 6 * 1024, NULL, 5, NULL, 1);
    return ESP_OK;
}

void app_player_play(int book_idx, int chapter)
{
    player_cmd_t c = { .type = CMD_PLAY, .book_idx = book_idx, .chapter = chapter };
    xQueueSend(cmd_q, &c, 0);
}
void app_player_toggle_pause(void) { player_cmd_t c = { .type = CMD_PAUSE_TOGGLE }; xQueueSend(cmd_q, &c, 0); }
void app_player_stop(void)         { player_cmd_t c = { .type = CMD_STOP };  xQueueSend(cmd_q, &c, 0); }
void app_player_next(void)         { player_cmd_t c = { .type = CMD_NEXT };  xQueueSend(cmd_q, &c, 0); }
void app_player_prev(void)         { player_cmd_t c = { .type = CMD_PREV };  xQueueSend(cmd_q, &c, 0); }
void app_player_seek_ms(uint32_t ms) { player_cmd_t c = { .type = CMD_SEEK, .arg = ms }; xQueueSend(cmd_q, &c, 0); }
void app_player_click(void)          { player_cmd_t c = { .type = CMD_CLICK }; xQueueSend(cmd_q, &c, 0); }

void app_player_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.volume = vol; xSemaphoreGive(st_mux);
    if (cur_sr) esp_codec_dev_set_out_vol(playback, (float)vol);
}
int app_player_get_volume(void) { int v; xSemaphoreTake(st_mux, portMAX_DELAY); v = g_st.volume; xSemaphoreGive(st_mux); return v; }
void app_player_set_repeat_all(bool en) { xSemaphoreTake(st_mux, portMAX_DELAY); g_st.repeat_all = en; xSemaphoreGive(st_mux); }
void app_player_set_event_cb(player_event_cb_t cb) { evt_cb = cb; }
void app_player_set_amp_cb(player_amp_cb_t cb) { amp_cb = cb; }
