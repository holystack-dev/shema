// app_player — Helix MP3 decode -> esp_codec_dev (ES8311 via codec_board).
#include "app_player.h"
#include "ui_click.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_codec_dev.h"
#include "esp_random.h"
#include "codec_board.h"
#include "codec_init.h"
#include "mp3dec.h"
#include "bible_data.h"
#include "app_played.h"

static const char *TAG = "player";

#define SD_MOUNT       "/sdcard"
#define INBUF_SZ       (4 * 1024)
#define MAX_PCM_SAMP   (MAX_NGRAN * MAX_NSAMP * MAX_NCHAN)   // 2304 shorts
#define MAX_MONO_SAMP  (MAX_NGRAN * MAX_NSAMP)               // 1152 samples/frame

// All player mutations run on the player task via this queue, so nothing else
// touches the codec, the open file, or g_base/name_fmt (single-writer model).
typedef enum {
    CMD_PLAY, CMD_PLAY_BIY, CMD_PLAY_LIB, CMD_PAUSE_TOGGLE, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_SEEK, CMD_CLICK,
    CMD_SET_BASE, CMD_SET_VOLUME, CMD_SUSPEND, CMD_PLAY_LIB_SHUF,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int        book_idx;
    int        chapter;
    int        biy_day;    // BIY: 1..365, or <0
    int        biy_intro;  // BIY: 0-based intro index, or <0
    uint32_t   arg;     // seek target ms / volume 0..100
} player_cmd_t;

// ---- shared state (guarded by st_mux) ----
static SemaphoreHandle_t st_mux;
static player_status_t   g_st = { .state = PLAYER_STOPPED, .kind = TRACK_BIBLE,
                                  .book_idx = -1, .biy_day = -1, .biy_intro = -1,
                                  .lib_idx = -1, .volume = 70 };

static QueueHandle_t      cmd_q;
static SemaphoreHandle_t  suspend_done;   // player task -> app_player_suspend() ack
static esp_codec_dev_handle_t playback;
static HMP3Decoder        mp3;
static player_event_cb_t  evt_cb;
static player_amp_cb_t    amp_cb;

static void amp(bool on) { if (amp_cb) amp_cb(on); }

// Post a command with a short timeout instead of dropping on a full queue. The
// player task only stalls for a single frame (~26 ms) inside esp_codec_dev_write,
// so 100 ms is ample; a genuine failure is logged rather than silently lost.
static void post_cmd(const player_cmd_t *c)
{
    if (cmd_q && xQueueSend(cmd_q, c, pdMS_TO_TICKS(100)) != pdTRUE)
        ESP_LOGW(TAG, "cmd queue full; dropped cmd type %d", (int)c->type);
}

// ---- player-task-local ----
static FILE     *fp;
static long      track_size;
static long      track_start;   // byte offset of the first audio frame (past ID3v2/cover art)
static uint8_t   in_buf[INBUF_SZ];
static uint8_t  *in_ptr;
static int       in_left;
static short     pcm[MAX_PCM_SAMP];
static short     pcm_stereo[MAX_MONO_SAMP * 2];
static int       cur_sr, cur_ch;          // currently opened codec format
static uint64_t  pos_samples;             // decoded samples (per channel)
static int       name_fmt = -1;           // 0: "%d_%d", 1: "%02d_%02d"

void app_player_get_status(player_status_t *out)
{
    if (!st_mux) {                 // called before app_player_init(): report "stopped"
        *out = (player_status_t){ .state = PLAYER_STOPPED, .book_idx = -1 };
        return;
    }
    xSemaphoreTake(st_mux, portMAX_DELAY);
    *out = g_st;
    xSemaphoreGive(st_mux);
}

static bool is_dir(const char *p) { DIR *d = opendir(p); if (d) { closedir(d); return true; } return false; }

// ---- audio source: BIBLE/<LANG>/<VERSION>/<book>_<chapter>.mp3 on the SD card ----
#define VER_ROOT   SD_MOUNT "/BIBLE"
#define MAX_VERS   12
typedef struct { char dir[96]; char name[40]; char shortn[32]; } ver_t;
static ver_t g_vers[MAX_VERS];
static int   g_vers_n = -1;                  // -1 = not scanned yet
static char  g_base[96] = SD_MOUNT "/AUDIO"; // legacy default; player-task-only
static char  pending_base[96];               // UI->player handoff for CMD_SET_BASE (st_mux)

// File naming under the base, auto-detected: "1_1.mp3" vs "01_01.mp3".
static const char *const NAME_FMT[] = { "%s/%d_%d.mp3", "%s/%02d_%02d.mp3" };

static FILE *open_chapter(int book_id, int chapter)
{
    char path[160];
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
            char pretty[32]; int j = 0;                    // "POC_1" -> "POC 1"
            for (int i = 0; ve->d_name[i] && j < (int)sizeof(pretty) - 1; i++)
                pretty[j++] = (ve->d_name[i] == '_') ? ' ' : ve->d_name[i];
            pretty[j] = 0;
            // "POC 1 (ML)": version first, language as qualifier.
            snprintf(v->name, sizeof v->name, "%.28s (%.8s)", pretty, le->d_name);
            snprintf(v->shortn, sizeof v->shortn, "%.31s", pretty);                // "POC 1"
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
// Route the base change through the command queue so only the player task ever
// writes g_base/name_fmt — otherwise auto-advance could fopen() a half-updated
// path while the user picks a new version.
void app_player_set_base(const char *dir)
{
    if (!dir || !dir[0]) return;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    snprintf(pending_base, sizeof pending_base, "%s", dir);
    xSemaphoreGive(st_mux);
    player_cmd_t c = { .type = CMD_SET_BASE };
    post_cmd(&c);
}
// ---- Bible in a Year: BIY/<lang>/ "Day NNN - title.mp3" + "Intro NN - title.mp3".
//      Language sub-folders are discovered by name (like BIBLE/<LANG>), not hardcoded;
//      the folder name (e.g. "ML", "EN") is the on-screen label. ----
#define BIY_TITLE_MAX  56
#define BIY_DAY_MAX    365
#define BIY_MAX_INTRO  24
#define BIY_MAX_LANGS  8
#define BIY_ROOT       SD_MOUNT "/BIY"
typedef struct { char name[24]; char dir[48]; } biy_lang_t;
static biy_lang_t g_biy_langs[BIY_MAX_LANGS];
static int   g_biy_langs_n = -1;                        // -1 = not scanned yet
static char  g_biy_dir[48];                             // selected language's folder path
static int   g_biy_lang = -1;                           // selected index into g_biy_langs, or -1
static int   g_biy_day_n;
static char (*g_biy_day_title)[BIY_TITLE_MAX];          // [BIY_DAY_MAX+1], PSRAM; index by day 1..365
static char  g_biy_intro_title[BIY_MAX_INTRO][BIY_TITLE_MAX];
static int   g_biy_intro_n;

// Parse "Day 001 - In the Beginning.mp3" / "Intro 04 - The Early World.mp3".
static bool parse_biy_name(const char *n, bool *is_day, int *num, char *title, int tmax)
{
    const char *body;
    if (strncmp(n, "Day ", 4) == 0)       { *is_day = true;  body = n + 4; }
    else if (strncmp(n, "Intro ", 6) == 0){ *is_day = false; body = n + 6; }
    else return false;
    char *end;
    long v = strtol(body, &end, 10);
    if (end == body || strncmp(end, " - ", 3) != 0) return false;
    *num = (int)v;
    const char *t = end + 3;
    const char *dot = strrchr(t, '.');
    int len = dot ? (int)(dot - t) : (int)strlen(t);
    if (len < 0) len = 0;
    if (len > tmax - 1) len = tmax - 1;
    memcpy(title, t, len);
    title[len] = 0;
    return true;
}

// True if a folder holds at least one "Day .."/"Intro .." episode file. Used to skip
// empty language folders during the scan.
static bool biy_dir_has_episodes(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (strncmp(e->d_name, "Day ", 4) == 0 || strncmp(e->d_name, "Intro ", 6) == 0) { found = true; break; }
    }
    closedir(d);
    return found;
}

// Discover language sub-folders of /sdcard/BIY that actually contain episodes. The
// folder name is the on-screen label (e.g. "ML", "EN"). Scanned once, cached.
int app_player_biy_langs(void)
{
    if (g_biy_langs_n >= 0) return g_biy_langs_n;
    g_biy_langs_n = 0;
    DIR *ld = opendir(BIY_ROOT);
    if (!ld) { ESP_LOGW(TAG, "no %s on card", BIY_ROOT); return 0; }
    struct dirent *e;
    while ((e = readdir(ld)) != NULL && g_biy_langs_n < BIY_MAX_LANGS) {
        if (e->d_name[0] == '.') continue;
        char p[48]; snprintf(p, sizeof p, "%s/%.30s", BIY_ROOT, e->d_name);
        if (!is_dir(p) || !biy_dir_has_episodes(p)) continue;
        biy_lang_t *l = &g_biy_langs[g_biy_langs_n++];
        snprintf(l->name, sizeof l->name, "%.23s", e->d_name);
        snprintf(l->dir,  sizeof l->dir,  "%.47s", p);
    }
    closedir(ld);
    ESP_LOGI(TAG, "found %d BIY language(s) under %s", g_biy_langs_n, BIY_ROOT);
    return g_biy_langs_n;
}
const char *app_player_biy_lang_name(int i) { return (i >= 0 && i < g_biy_langs_n) ? g_biy_langs[i].name : ""; }
int app_player_biy_find_lang(const char *name)
{
    app_player_biy_langs();
    if (name) for (int i = 0; i < g_biy_langs_n; i++) if (strcmp(g_biy_langs[i].name, name) == 0) return i;
    return -1;
}

// Select a language by index into the scanned list; (re)scans that folder's Day/Intro
// titles. Returns the number of days found (0 on failure / bad index).
int app_player_biy_set_lang(int i)
{
    if (app_player_biy_langs() <= 0) return 0;
    if (i < 0 || i >= g_biy_langs_n) return 0;
    if (i == g_biy_lang && g_biy_day_n > 0) return g_biy_day_n;   // cached
    if (!g_biy_day_title)
        g_biy_day_title = heap_caps_calloc(BIY_DAY_MAX + 1, BIY_TITLE_MAX, MALLOC_CAP_SPIRAM);
    if (!g_biy_day_title) { ESP_LOGE(TAG, "BIY title alloc failed"); return 0; }
    memset(g_biy_day_title, 0, (BIY_DAY_MAX + 1) * BIY_TITLE_MAX);
    memset(g_biy_intro_title, 0, sizeof g_biy_intro_title);
    g_biy_day_n = 0; g_biy_intro_n = 0; g_biy_lang = i;
    snprintf(g_biy_dir, sizeof g_biy_dir, "%s", g_biy_langs[i].dir);
    DIR *d = opendir(g_biy_dir);
    if (!d) { ESP_LOGW(TAG, "no %s on card", g_biy_dir); return 0; }
    struct dirent *e;
    char title[BIY_TITLE_MAX];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        bool is_day; int num;
        if (!parse_biy_name(e->d_name, &is_day, &num, title, BIY_TITLE_MAX)) continue;
        if (is_day) {
            if (num >= 1 && num <= BIY_DAY_MAX) {
                strcpy(g_biy_day_title[num], title);
                g_biy_day_n++;
            }
        } else if (num >= 1 && num <= BIY_MAX_INTRO) {
            strcpy(g_biy_intro_title[num - 1], title);
            if (num > g_biy_intro_n) g_biy_intro_n = num;
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "BIY %s: %d days, %d intros", g_biy_dir, g_biy_day_n, g_biy_intro_n);
    return g_biy_day_n;
}
const char *app_player_biy_day_title(int day)
{
    if (!g_biy_day_title || day < 1 || day > BIY_DAY_MAX) return "";
    return g_biy_day_title[day];
}
int app_player_biy_intro_count(void) { return g_biy_intro_n; }
const char *app_player_biy_intro_title(int i)
{
    return (i >= 0 && i < g_biy_intro_n) ? g_biy_intro_title[i] : "";
}

static FILE *open_biy(int day, int intro)
{
    char path[200];
    if (intro >= 0)
        snprintf(path, sizeof(path), "%s/Intro %02d - %s.mp3", g_biy_dir, intro + 1, g_biy_intro_title[intro]);
    else
        snprintf(path, sizeof(path), "%s/Day %03d - %s.mp3", g_biy_dir, day, g_biy_day_title[day]);
    return fopen(path, "rb");
}

// ---- Library: LIBRARY/<folder>/.../<track>.mp3 — generic nested folder browser ----
#define LIB_ROOT       SD_MOUNT "/LIBRARY"
#define LIB_NAME_MAX   80
#define LIB_PATH_MAX   192
#define LIB_MAX_ENTS   1500          // max entries (folders + tracks) in one folder

// UI-task-only browse cache. Keep four recently visited folders so Back doesn't
// rescan the parent. Buffers grow with actual folder size, with a bounded PSRAM
// maximum of four LIB_MAX_ENTS listings; playback owns a separate track table.
typedef struct {
    char name[LIB_NAME_MAX];
    bool is_dir, done;
    int track;                              // file's index, -1 for a folder
    uint32_t done_revision;
} lib_ent_t;
#define LIB_CACHE_SLOTS 4
typedef struct {
    char rel[LIB_PATH_MAX];
    lib_ent_t *entries;
    int count, capacity;
    uint32_t age;
    bool valid;
} lib_cache_t;
static lib_cache_t g_lib_cache[LIB_CACHE_SLOTS];
static uint32_t g_lib_cache_age;
static lib_ent_t *g_lib_ents;                 // aliases the active cached listing
static int   g_lib_ents_n;
static char  g_lib_browsed[LIB_PATH_MAX];     // rel path currently in the browse cache
// Play context (player task only): the tracks of the folder that is *playing*, kept
// separate so auto-advance stays correct even while the user browses elsewhere.
//
// Entries are paths relative to g_libp_dir ("song.mp3", or "Sub/song.mp3" for a
// recursive shuffle), hence LIB_PATH_MAX wide.
static char (*g_libp_tracks)[LIB_PATH_MAX];   // PSRAM [LIB_MAX_ENTS], names keep ".mp3"
static int   g_libp_n;
static char  g_libp_dir[LIB_PATH_MAX];        // rel path of the playing folder ("" = LIBRARY root)
static char  pending_lib[LIB_PATH_MAX];       // UI->player handoff for CMD_PLAY_LIB (st_mux)

// Shuffle (Library only). g_libp_order is a permutation of 0..g_libp_n-1 and
// g_libp_pos is the cursor into it; Next/Prev step through the permutation.
static uint16_t *g_libp_order;                // PSRAM [LIB_MAX_ENTS]
static int   g_libp_pos;
static bool  g_libp_shuffle;

static bool has_mp3_ext(const char *n)
{
    size_t L = strlen(n);
    return L > 4 && strcasecmp(n + L - 4, ".mp3") == 0;
}
static int name_cmp(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
static int ent_cmp(const void *a, const void *b)
{
    const lib_ent_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return (int)y->is_dir - (int)x->is_dir;   // folders first
    return strcasecmp(x->name, y->name);                                  // then by full name
}
static void lib_abs(char *out, int sz, const char *rel)
{
    if (rel && rel[0]) snprintf(out, sz, "%s/%s", LIB_ROOT, rel);
    else               snprintf(out, sz, "%s", LIB_ROOT);
}

bool app_player_has_library(void)
{
    static int cached = -1;
    if (cached < 0) cached = is_dir(LIB_ROOT) ? 1 : 0;
    return cached != 0;
}

int app_player_lib_browse(const char *rel)
{
    if (!rel) rel = "";
    lib_cache_t *slot = NULL;
    for (int i = 0; i < LIB_CACHE_SLOTS; i++) {
        lib_cache_t *candidate = &g_lib_cache[i];
        if (candidate->valid && strcmp(rel, candidate->rel) == 0) {
            candidate->age = ++g_lib_cache_age;
            g_lib_ents = candidate->entries;
            g_lib_ents_n = candidate->count;
            snprintf(g_lib_browsed, sizeof g_lib_browsed, "%s", rel);
            return g_lib_ents_n;
        }
        if (!slot || (!candidate->valid && slot->valid) ||
            (candidate->valid == slot->valid && candidate->age < slot->age)) slot = candidate;
    }
    // Failed opens must not poison another folder's cached entry count.
    g_lib_ents = NULL;
    g_lib_ents_n = 0;
    snprintf(g_lib_browsed, sizeof g_lib_browsed, "%s", rel);
    char abs[LIB_PATH_MAX + 16]; lib_abs(abs, sizeof abs, rel);
    DIR *d = opendir(abs);
    if (!d) { ESP_LOGW(TAG, "no %s on card", abs); return 0; }
    slot->valid = false;
    slot->count = 0;
    bool complete = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && slot->count < LIB_MAX_ENTS) {
        if (e->d_name[0] == '.') continue;
        // Use FATFS's dirent type (DT_DIR/DT_REG) instead of an opendir() per entry — a
        // 900+ file folder would otherwise fire hundreds of SD opendir()s, contending with
        // the MP3 stream on the same card and stalling playback.
        bool dir = (e->d_type == DT_DIR);
        if (!dir && !has_mp3_ext(e->d_name)) continue;           // only folders + mp3 tracks
        if (slot->count == slot->capacity) {
            int capacity = slot->capacity ? slot->capacity * 2 : 32;
            if (capacity > LIB_MAX_ENTS) capacity = LIB_MAX_ENTS;
            lib_ent_t *grown = heap_caps_realloc(slot->entries, (size_t)capacity * sizeof(lib_ent_t), MALLOC_CAP_SPIRAM);
            if (!grown) { complete = false; ESP_LOGE(TAG, "library cache alloc failed"); break; }
            slot->entries = grown;
            slot->capacity = capacity;
        }
        lib_ent_t *le = &slot->entries[slot->count++];
        *le = (lib_ent_t){.is_dir = dir, .track = -1};
        snprintf(le->name, sizeof le->name, "%.79s", e->d_name); // keep ".mp3"; stripped at display
    }
    closedir(d);
    if (!complete) return 0;          // don't expose a partial, differently indexed play set
    g_lib_ents = slot->entries;
    g_lib_ents_n = slot->count;
    snprintf(slot->rel, sizeof slot->rel, "%s", rel);
    slot->valid = true;
    slot->age = ++g_lib_cache_age;
    if (g_lib_ents_n > 1) qsort(g_lib_ents, g_lib_ents_n, sizeof(lib_ent_t), ent_cmp);
    int t = 0;
    for (int i = 0; i < g_lib_ents_n; i++) if (!g_lib_ents[i].is_dir) g_lib_ents[i].track = t++;
    ESP_LOGI(TAG, "library %s: %d entries (%d tracks)", rel[0] ? rel : "(root)", g_lib_ents_n, t);
    return g_lib_ents_n;
}
int  app_player_lib_count(void) { return g_lib_ents_n; }
bool app_player_lib_is_dir(int i) { return (i >= 0 && i < g_lib_ents_n) && g_lib_ents[i].is_dir; }
int  app_player_lib_track_of(int i) { return (i >= 0 && i < g_lib_ents_n) ? g_lib_ents[i].track : -1; }
// ---- folder-level "all played" ----
//
// Stops at the first unfinished track. Folders with no finished descendants in the
// in-memory table need no SD reads. Results are cached with the listing until
// completion changes. An incomplete or failed scan never reports "finished".
#define LIB_SCAN_CAP   800
#define LIB_SCAN_DEPTH 3

static bool lib_all_done(const char *abs, const char *rel, int depth,
                         int *visited, bool *any_track)
{
    if (depth > LIB_SCAN_DEPTH) return false;
    DIR *d = opendir(abs);
    if (!d) return false;
    bool done = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (++(*visited) > LIB_SCAN_CAP) { done = false; break; }
        char a[LIB_PATH_MAX + 128], r[LIB_PATH_MAX];
        int alen = snprintf(a, sizeof a, "%s/%s", abs, e->d_name);
        int rlen = snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        if (alen >= sizeof a || rlen >= sizeof r) { done = false; break; }
        bool dir = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && is_dir(a));
        if (dir) {
            if (!lib_all_done(a, r, depth + 1, visited, any_track)) { done = false; break; }
        } else if (has_mp3_ext(e->d_name)) {
            *any_track = true;
            if (!app_played_get_lib(r, NULL)) { done = false; break; }
        }
    }
    closedir(d);
    return done;
}

// True when browsed entry `i` is a folder whose every track has been finished.
bool app_player_lib_folder_done(int i)
{
    if (i < 0 || i >= g_lib_ents_n || !g_lib_ents[i].is_dir) return false;
    lib_ent_t *entry = &g_lib_ents[i];
    uint32_t revision = app_played_lib_revision();
    if (entry->done_revision == revision) return entry->done;
    char abs[LIB_PATH_MAX + 128], rel[LIB_PATH_MAX];
    int len = g_lib_browsed[0] ? snprintf(rel, sizeof rel, "%s/%s", g_lib_browsed, entry->name)
                               : snprintf(rel, sizeof rel, "%s", entry->name);
    bool done = false;
    if (len < sizeof rel && app_played_lib_folder_has_done(rel)) {
        snprintf(abs, sizeof abs, "%s/%s", LIB_ROOT, rel);
        int visited = 0;
        bool any_track = false;
        done = lib_all_done(abs, rel, 0, &visited, &any_track) && any_track;
    }
    entry->done = done;
    entry->done_revision = revision;
    return done;
}

// Identity of a BROWSED entry, as a path relative to LIBRARY/ — the same key
// app_played stores. Distinct from lib_rel_of(), which works off the PLAYING set;
// the two differ whenever the user browses away from what is playing.
bool app_player_lib_rel(int i, char *out, size_t out_sz)
{
    if (i < 0 || i >= g_lib_ents_n || g_lib_ents[i].is_dir || !out || out_sz == 0) return false;
    if (g_lib_browsed[0]) snprintf(out, out_sz, "%s/%s", g_lib_browsed, g_lib_ents[i].name);
    else                  snprintf(out, out_sz, "%s", g_lib_ents[i].name);
    return out[0] != 0;
}

const char *app_player_lib_name(int i)
{
    if (i < 0 || i >= g_lib_ents_n) return "";
    if (g_lib_ents[i].is_dir) return g_lib_ents[i].name;
    static char disp[LIB_NAME_MAX];                              // strip ".mp3" for tracks
    snprintf(disp, sizeof disp, "%s", g_lib_ents[i].name);
    size_t L = strlen(disp);
    if (L > 4 && strcasecmp(disp + L - 4, ".mp3") == 0) disp[L - 4] = 0;
    return disp;
}

static bool libp_alloc(void)
{
    if (!g_libp_tracks)
        g_libp_tracks = heap_caps_malloc((size_t)LIB_MAX_ENTS * LIB_PATH_MAX, MALLOC_CAP_SPIRAM);
    if (!g_libp_tracks) { ESP_LOGE(TAG, "library play alloc failed"); return false; }
    return true;
}

// Load folder `rel`'s tracks into the play context (player task). Cached by dir.
static bool lib_load_play(const char *rel)
{
    // A recursive (shuffle) load leaves the same dir with a different track set, so the
    // cache check has to know which kind of load produced it.
    if (!g_libp_shuffle && strcmp(rel, g_libp_dir) == 0 && g_libp_n > 0) return true;
    if (!libp_alloc()) return false;
    g_libp_n = 0;
    g_libp_shuffle = false;
    char abs[LIB_PATH_MAX + 16]; lib_abs(abs, sizeof abs, rel);
    DIR *d = opendir(abs);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_libp_n < LIB_MAX_ENTS) {
        if (e->d_name[0] == '.' || !has_mp3_ext(e->d_name)) continue;
        // FATFS long names run to 255 bytes; bound them explicitly to the entry width.
        snprintf(g_libp_tracks[g_libp_n++], LIB_PATH_MAX, "%.*s", LIB_PATH_MAX - 1, e->d_name);
    }
    closedir(d);
    qsort(g_libp_tracks, g_libp_n, LIB_PATH_MAX, name_cmp);       // same order as browse tracks
    snprintf(g_libp_dir, sizeof g_libp_dir, "%s", rel);
    return true;
}

// Recursively collect every .mp3 under `root`, storing each as a path relative to
// `root`. `sub` is the current sub-path ("" at the top). Depth-bounded so a symlink
// loop or a pathologically nested card cannot blow the stack.
static void lib_collect(const char *root, const char *sub, int depth)
{
    if (depth > 6 || g_libp_n >= LIB_MAX_ENTS) return;
    // Every component is bounded to HALF the buffer so "a/b" can never overflow it. A
    // path that would not fit is skipped rather than silently truncated into a name that
    // points at nothing.
    enum { HALF = LIB_PATH_MAX / 2 - 1 };
    char rel[LIB_PATH_MAX];
    if (sub[0]) snprintf(rel, sizeof rel, "%.*s/%.*s", HALF, root, HALF, sub);
    else        snprintf(rel, sizeof rel, "%.*s", LIB_PATH_MAX - 1, root);
    char abs[LIB_PATH_MAX + 16]; lib_abs(abs, sizeof abs, rel);

    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_libp_n < LIB_MAX_ENTS) {
        if (e->d_name[0] == '.') continue;
        if (strlen(e->d_name) > (size_t)HALF) continue;     // unreasonably long: skip it
        char child[LIB_PATH_MAX];
        if (sub[0]) snprintf(child, sizeof child, "%.*s/%.*s", HALF, sub, HALF, e->d_name);
        else        snprintf(child, sizeof child, "%.*s", HALF, e->d_name);
        if (has_mp3_ext(e->d_name)) {
            snprintf(g_libp_tracks[g_libp_n++], LIB_PATH_MAX, "%.*s", LIB_PATH_MAX - 1, child);
        } else {
            char probe[LIB_PATH_MAX * 2];
            snprintf(probe, sizeof probe, "%.*s/%.*s", LIB_PATH_MAX + 8, abs, HALF, e->d_name);
            if (is_dir(probe)) {
                closedir(d);                       // one open DIR per level: FATFS is tight
                lib_collect(root, child, depth + 1);
                d = opendir(abs);
                if (!d) return;
                // Re-scan from the top to the current entry (no telldir/seekdir dependency).
                struct dirent *r;
                while ((r = readdir(d)) != NULL && strcmp(r->d_name, e->d_name) != 0) { }
            }
        }
    }
    if (d) closedir(d);
}

// Build a shuffled play order over the whole loaded set (Fisher-Yates, hardware RNG).
static void lib_shuffle_order(void)
{
    if (!g_libp_order)
        g_libp_order = heap_caps_malloc((size_t)LIB_MAX_ENTS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_libp_order) { ESP_LOGE(TAG, "shuffle order alloc failed"); g_libp_shuffle = false; return; }
    for (int i = 0; i < g_libp_n; i++) g_libp_order[i] = (uint16_t)i;
    for (int i = g_libp_n - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint16_t t = g_libp_order[i]; g_libp_order[i] = g_libp_order[j]; g_libp_order[j] = t;
    }
    g_libp_pos = 0;
}

// Load every track under `rel` (including sub-folders) and shuffle them.
static bool lib_load_shuffled(const char *rel)
{
    if (!libp_alloc()) return false;
    g_libp_n = 0;
    lib_collect(rel, "", 0);
    if (g_libp_n <= 0) { ESP_LOGW(TAG, "shuffle: no tracks under %s", rel[0] ? rel : "(root)"); return false; }
    snprintf(g_libp_dir, sizeof g_libp_dir, "%s", rel);
    g_libp_shuffle = true;
    lib_shuffle_order();
    ESP_LOGI(TAG, "shuffle: %d track(s) under %s", g_libp_n, rel[0] ? rel : "(root)");
    return true;
}

static FILE *open_lib(int idx)
{
    if (idx < 0 || idx >= g_libp_n) return NULL;
    char path[LIB_PATH_MAX + LIB_NAME_MAX + 16];
    if (g_libp_dir[0]) snprintf(path, sizeof path, "%s/%s/%s", LIB_ROOT, g_libp_dir, g_libp_tracks[idx]);
    else               snprintf(path, sizeof path, "%s/%s", LIB_ROOT, g_libp_tracks[idx]);
    return fopen(path, "rb");
}

const char *app_player_lib_playing_name(void)
{
    int i = g_st.lib_idx;
    if (g_st.kind != TRACK_LIB || i < 0 || i >= g_libp_n) return "";
    // A shuffled entry can be "Sub/Folder/song.mp3"; show only the file name.
    const char *base = strrchr(g_libp_tracks[i], '/');
    base = base ? base + 1 : g_libp_tracks[i];
    static char disp[LIB_NAME_MAX];
    snprintf(disp, sizeof disp, "%s", base);
    size_t L = strlen(disp);
    if (L > 4 && strcasecmp(disp + L - 4, ".mp3") == 0) disp[L - 4] = 0;
    return disp;
}
const char *app_player_lib_playing_folder(void)
{
    // While shuffling, the useful label is the folder the CURRENT track actually lives
    // in, not the root the shuffle was started from — otherwise every track in a
    // recursive shuffle claims to be in the same place.
    int i = g_st.lib_idx;
    if (g_libp_shuffle && g_st.kind == TRACK_LIB && i >= 0 && i < g_libp_n) {
        const char *slash = strrchr(g_libp_tracks[i], '/');
        if (slash) {
            static char sub[LIB_NAME_MAX];
            size_t n = (size_t)(slash - g_libp_tracks[i]);
            if (n >= sizeof sub) n = sizeof sub - 1;
            memcpy(sub, g_libp_tracks[i], n);
            sub[n] = 0;
            const char *last = strrchr(sub, '/');
            return last ? last + 1 : sub;
        }
    }
    if (!g_libp_dir[0]) return "Library";
    const char *s = strrchr(g_libp_dir, '/');
    return s ? s + 1 : g_libp_dir;
}
const char *app_player_lib_playing_dir(void) { return g_libp_dir; }   // full rel path, for resume

static int refill(void)
{
    if (!fp) return 0;
    if (in_left > 0 && in_ptr != in_buf) memmove(in_buf, in_ptr, in_left);
    in_ptr = in_buf;
    int got = fread(in_buf + in_left, 1, INBUF_SZ - in_left, fp);
    if (got > 0) in_left += got;
    else if (ferror(fp)) ESP_LOGW(TAG, "SD read error mid-chapter");   // e.g. card pulled
    return got;
}

static void close_track(void)
{
    if (fp) { fclose(fp); fp = NULL; }
    in_ptr = in_buf;
    in_left = 0;
}

// fp must be open. Set track_size, reset position, and compute duration straight
// from the MP3 (CBR bitrate x audio size). Works for every version/recording and
// for BIY, unlike a table baked from one source. Skip any ID3v2 tag, read the
// first frame header for the bitrate, then rewind for the decode loop.
static uint32_t begin_decode(void)
{
    fseek(fp, 0, SEEK_END);
    track_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    pos_samples = 0;

    uint32_t dur = 0;
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
    // Start decoding AT the first audio frame, not byte 0 — otherwise the decoder
    // chews through the ID3v2 tag (BIY files embed a big PNG cover) and emits
    // clicks/noise from false sync words at the head of every track.
    track_start = start + (off >= 0 ? off : 0);
    fseek(fp, track_start, SEEK_SET);
    in_ptr = in_buf; in_left = 0;
    return dur;
}

// ---- per-track progress (app_played, persisted on the SD card) ----
//
// Each track has its own resume point (separate from app_store's single "last played"
// position). Progress is written at track boundaries — start, natural end, stop,
// power-off — never on the 5 s position tick.
static void do_seek(uint32_t ms);   // defined below, after the decode helpers

// Rebuild a Library track's identity: a path relative to LIBRARY/. Entries are already
// relative to the playing folder (a recursive shuffle stores "Sub/song.mp3"), so the two
// simply concatenate.
static bool lib_rel_of(int idx, char *out, size_t out_sz)
{
    if (idx < 0 || idx >= g_libp_n || !g_libp_tracks) return false;
    if (g_libp_dir[0]) snprintf(out, out_sz, "%s/%s", g_libp_dir, g_libp_tracks[idx]);
    else               snprintf(out, out_sz, "%s", g_libp_tracks[idx]);
    return out[0] != 0;
}

// Set by the end-of-track path so the start of the NEXT track does not immediately
// overwrite the "finished" record with a stale position.
static bool progress_done_pending;

static void save_progress(bool done)
{
    player_status_t s;
    xSemaphoreTake(st_mux, portMAX_DELAY); s = g_st; xSemaphoreGive(st_mux);
    if (!done && s.state == PLAYER_STOPPED) return;

    switch (s.kind) {
    case TRACK_BIBLE:
        if (s.book_idx >= 0 && s.chapter >= 1)
            app_played_set_bible(s.book_idx, s.chapter, s.pos_ms, done);
        break;
    case TRACK_BIY:
        if (s.biy_day >= 1 || s.biy_intro >= 0)
            app_played_set_biy(s.biy_day, s.biy_intro, s.pos_ms, done);
        break;
    case TRACK_LIB: {
        char rel[LIB_PATH_MAX];
        if (lib_rel_of(s.lib_idx, rel, sizeof rel))
            app_played_set_lib(rel, s.pos_ms, done);
        break;
    }
    }
}

// Called at the top of every start_*: bank the outgoing track's position before its
// identity is replaced. Skipped once if that track just ended naturally.
static void save_outgoing(void)
{
    if (progress_done_pending) { progress_done_pending = false; return; }
    save_progress(false);
}

// Jump to a stored resume point. Ignored within RESUME_EDGE_MS of either end.
#define RESUME_EDGE_MS 3000u
static void resume_to(uint32_t pos_ms, uint32_t dur_ms)
{
    if (pos_ms <= RESUME_EDGE_MS) return;
    if (dur_ms == 0 || pos_ms + RESUME_EDGE_MS >= dur_ms) return;
    do_seek(pos_ms);
    ESP_LOGI(TAG, "resuming at %lu ms", (unsigned long)pos_ms);
}

static bool start_track(int book_idx, int chapter)
{
    save_outgoing();
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
    uint32_t dur = begin_decode();
    if (track_size <= 0) {                    // unreadable / empty file -> treat as missing
        ESP_LOGE(TAG, "bad size (%ld) for book %d ch %d",
                 track_size, BIBLE_BOOKS[book_idx].id, chapter);
        close_track();
        return false;
    }
    int vol;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    g_st.state = PLAYER_PLAYING; g_st.kind = TRACK_BIBLE;
    g_st.book_idx = book_idx; g_st.chapter = chapter;
    g_st.biy_day = -1; g_st.biy_intro = -1; g_st.lib_idx = -1;
    g_st.pos_ms = 0; g_st.dur_ms = dur; vol = g_st.volume;
    xSemaphoreGive(st_mux);

    ESP_LOGI(TAG, "play %s %d (%ld bytes, %lu ms) vol=%d",
             BIBLE_BOOKS[book_idx].name, chapter, track_size, (unsigned long)dur, vol);
    uint32_t rpos = 0;
    app_played_get_bible(book_idx, chapter, &rpos);
    resume_to(rpos, dur);
    amp(true);   // enable amp before any PCM is written (no clipped start)
    return true;
}

// day>=1 plays a daily episode; intro>=0 plays an intro/checkpoint (day ignored).
static bool start_track_biy(int day, int intro)
{
    save_outgoing();
    close_track();
    if (g_biy_lang < 0) return false;
    if (intro < 0 && (day < 1 || day > BIY_DAY_MAX || !g_biy_day_title[day][0])) return false;
    if (intro >= 0 && (intro >= g_biy_intro_n || !g_biy_intro_title[intro][0])) return false;

    fp = open_biy(day, intro);
    if (!fp) { ESP_LOGE(TAG, "missing BIY audio: day=%d intro=%d", day, intro); return false; }
    uint32_t dur = begin_decode();
    if (track_size <= 0) {                    // unreadable / empty file -> treat as missing
        ESP_LOGE(TAG, "bad size (%ld) for BIY day=%d intro=%d", track_size, day, intro);
        close_track();
        return false;
    }
    int vol;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    g_st.state = PLAYER_PLAYING; g_st.kind = TRACK_BIY;
    g_st.book_idx = -1; g_st.chapter = 0;
    g_st.biy_day = (intro < 0) ? day : -1; g_st.biy_intro = intro; g_st.lib_idx = -1;
    g_st.pos_ms = 0; g_st.dur_ms = dur; vol = g_st.volume;
    xSemaphoreGive(st_mux);

    ESP_LOGI(TAG, "play BIY %s (%ld bytes, %lu ms) vol=%d",
             intro < 0 ? g_biy_day_title[day] : g_biy_intro_title[intro],
             track_size, (unsigned long)dur, vol);
    uint32_t rpos = 0;
    app_played_get_biy((intro < 0) ? day : -1, intro, &rpos);
    resume_to(rpos, dur);
    amp(true);
    return true;
}

// Play g_libp_tracks[idx] from the already-loaded play set. Does not call
// lib_load_play(), so a shuffled order is preserved.
static bool start_lib_idx(int idx)
{
    save_outgoing();
    close_track();
    if (idx < 0 || idx >= g_libp_n) return false;
    const char *rel = g_libp_dir;
    fp = open_lib(idx);
    if (!fp) { ESP_LOGE(TAG, "missing library track: %s[%d]", rel[0] ? rel : "(root)", idx); return false; }
    uint32_t dur = begin_decode();
    if (track_size <= 0) {                    // unreadable / empty file -> treat as missing
        ESP_LOGE(TAG, "bad size (%ld) for library %s[%d]", track_size, rel[0] ? rel : "(root)", idx);
        close_track();
        return false;
    }
    int vol;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    g_st.state = PLAYER_PLAYING; g_st.kind = TRACK_LIB;
    g_st.book_idx = -1; g_st.chapter = 0; g_st.biy_day = -1; g_st.biy_intro = -1;
    g_st.lib_idx = idx;
    g_st.pos_ms = 0; g_st.dur_ms = dur; vol = g_st.volume;
    xSemaphoreGive(st_mux);

    ESP_LOGI(TAG, "play LIB %s / %s (%ld bytes, %lu ms)%s vol=%d",
             rel[0] ? rel : "(root)", g_libp_tracks[idx], track_size, (unsigned long)dur,
             g_libp_shuffle ? " [shuffle]" : "", vol);
    char lrel[LIB_PATH_MAX];
    uint32_t rpos = 0;
    if (lib_rel_of(idx, lrel, sizeof lrel)) app_played_get_lib(lrel, &rpos);
    resume_to(rpos, dur);
    amp(true);
    return true;
}

// Play track `idx` of folder `rel`, loading that folder sequentially. Used when the
// reader picks a specific track, which is also how shuffle gets switched off.
static bool start_track_lib(const char *rel, int idx)
{
    if (!lib_load_play(rel)) return false;
    return start_lib_idx(idx);
}

// Shuffle everything under `rel` (that folder and all its sub-folders) and start.
static bool start_track_lib_shuffled(const char *rel)
{
    if (!lib_load_shuffled(rel)) return false;
    return start_lib_idx(g_libp_order[g_libp_pos]);
}

// Volume curve: slider percent -> dB. The codec's default two-point map (-50..0 dB,
// linear in dB) leaves 50% nearly inaudible. Points tuned by ear on this speaker;
// 100% stays at 0 dB.
static const esp_codec_dev_vol_map_t VOL_CURVE[] = {
    {   0, -50.0f },
    {  10, -24.0f },
    {  25, -15.0f },
    {  50,  -8.0f },
    {  75,  -3.0f },
    { 100,   0.0f },
};

// Last volume written to the codec; the player loop re-applies g_st.volume if they differ.
static int vol_applied = -1;

// Open the codec at (sr, ch); returns false if the open failed. cur_sr/cur_ch are
// latched only on success, so a failed open is retried on the next call.
static bool ensure_codec(int sr, int ch)
{
    if (sr == cur_sr && ch == cur_ch) return true;
    if (cur_sr) { esp_codec_dev_close(playback); cur_sr = cur_ch = 0; }  // now closed
    esp_codec_dev_sample_info_t fs = { .sample_rate = sr, .channel = 2, .bits_per_sample = 16 };
    int rc = esp_codec_dev_open(playback, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed (%d) at %d Hz", rc, sr);
        vol_applied = -1;   // hardware state unknown; force a re-apply once it opens
        return false;   // leave cur_sr/cur_ch = 0 so the next attempt retries the open
    }
    // Re-applied on every open: the curve lives in the codec_dev instance and the open
    // path re-pushes dev->volume through whatever curve is installed at that moment.
    esp_codec_dev_vol_curve_t curve = { .vol_map = (esp_codec_dev_vol_map_t *)VOL_CURVE,
                                        .count = sizeof(VOL_CURVE) / sizeof(VOL_CURVE[0]) };
    esp_codec_dev_set_vol_curve(playback, &curve);
    int vol;
    xSemaphoreTake(st_mux, portMAX_DELAY); vol = g_st.volume; xSemaphoreGive(st_mux);
    esp_codec_dev_set_out_vol(playback, vol);
    vol_applied = vol;
    cur_sr = sr; cur_ch = ch;
    ESP_LOGI(TAG, "Open codec device OK %d Hz", sr);
    return true;
}

// returns: 0 decoded a frame, 1 EOF, -1 fatal
static int decode_frame(void)
{
    static int underflow_eof;   // consecutive underflows with no new bytes from the card
    static int codec_fail;      // consecutive codec-open failures

    if (in_left < MAINBUF_SIZE) {
        if (refill() == 0 && in_left == 0) return 1; // EOF, nothing buffered
    }
    int off = MP3FindSyncWord(in_ptr, in_left);
    if (off < 0) {
        // No sync in the buffer. Keep the last few bytes in case a sync word straddles
        // the buffer boundary — dropping the whole buffer could split a 2-4 byte sync
        // and glitch one frame. When the card has no more bytes, that's EOF.
        int keep = (in_left >= 3) ? 3 : in_left;
        in_ptr += in_left - keep;
        in_left = keep;
        return (refill() == 0) ? 1 : 0;
    }
    in_ptr += off; in_left -= off;
    if (in_left < MAINBUF_SIZE) refill();

    int err = MP3Decode(mp3, &in_ptr, &in_left, pcm, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
        if (refill() > 0) { underflow_eof = 0; return 0; }   // got more file data, retry
        // No more bytes on the card. Helix underflow consumes no input, so a truncated
        // final frame (valid sync, short body) leaves in_left > 0 forever: the same
        // frame underflows every pass and busy-spins core 1. Bail once the buffer
        // is drained or has stopped growing across a couple of passes.
        if (in_left == 0 || ++underflow_eof >= 2) { underflow_eof = 0; return 1; }
        return 0;
    }
    underflow_eof = 0;
    if (err != ERR_MP3_NONE) {
        ESP_LOGW(TAG, "MP3 decode error: %d", err);
        if (in_left > 0) { in_ptr++; in_left--; }   // skip a byte, resync
        return 0;
    }

    MP3FrameInfo fi;
    MP3GetLastFrameInfo(mp3, &fi);
    if (fi.outputSamps <= 0) return 0;
    if (!ensure_codec(fi.samprate, fi.nChans)) {
        // Codec open failed (transient I2C/ES8311 glitch). Don't spin the CPU writing to
        // a closed codec; yield to pace the retry, and give up after a few tries so the
        // player stops cleanly instead of "playing" silently at 100% CPU.
        vTaskDelay(pdMS_TO_TICKS(20));
        return (++codec_fail >= 10) ? -1 : 0;
    }
    codec_fail = 0;

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
    ui_click_mix(out, nsamp, fi.samprate);
    esp_codec_dev_write(playback, out, out_bytes);

    pos_samples += nsamp;
    uint32_t ms = (uint32_t)(pos_samples * 1000 / (fi.samprate ? fi.samprate : 1));
    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.pos_ms = ms; xSemaphoreGive(st_mux);

    static uint32_t hb;
    if (++hb % 200 == 0) ESP_LOGI(TAG, "playing pos=%lu/%lu ms (%dHz/%dch)",
                                  (unsigned long)ms, (unsigned long)g_st.dur_ms, fi.samprate, fi.nChans);
    return 0;
}

// The player task owns both the decoder and feedback. During playback, mix a
// tick into the next frame. When paused/stopped, write it at the already-open
// codec rate, keeping the decoder, loaded track and resume position untouched.
static void play_click(void)
{
    ui_click_start();
    player_status_t st;
    app_player_get_status(&st);
    if (st.state == PLAYER_PLAYING && fp) return;
    if (!cur_sr && !ensure_codec(48000, 2)) { ui_click_cancel(); return; }
    int frames = (cur_sr + 99) / 100;   // 10 ms at the native rate
    if (frames > MAX_MONO_SAMP) { ui_click_cancel(); return; }
    memset(pcm_stereo, 0, frames * 2 * sizeof(short));
    ui_click_mix(pcm_stereo, frames, cur_sr);
    if (esp_codec_dev_write(playback, pcm_stereo, frames * 2 * sizeof(short)) != ESP_CODEC_DEV_OK)
        ESP_LOGW(TAG, "tap feedback write failed");
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

// Advance the current track by dir (+1 next, -1 prev), per its kind. Returns
// true if a new track started. BIY days step within 1..365 (no wrap); BIY intros
// are standalone (no next/prev/auto-advance).
static bool advance(int dir)
{
    track_kind_t kind; int bi, ch, day, intro, lib; bool ra;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    kind = g_st.kind; bi = g_st.book_idx; ch = g_st.chapter;
    day = g_st.biy_day; intro = g_st.biy_intro; lib = g_st.lib_idx; ra = g_st.repeat_all;
    xSemaphoreGive(st_mux);
    if (kind == TRACK_BIY) {
        if (intro >= 0) return false;
        int nd = day + dir;
        if (nd < 1 || nd > BIY_DAY_MAX) return false;
        return start_track_biy(nd, -1);
    }
    if (kind == TRACK_LIB) {
        if (g_libp_shuffle && g_libp_order && g_libp_n > 0) {
            // Step through the permutation, not the folder order. Wraps: a shuffle is a
            // continuous listening mode, so reaching the end starts the set again.
            int np = g_libp_pos + dir;
            if (np < 0) np = g_libp_n - 1;
            else if (np >= g_libp_n) np = 0;
            g_libp_pos = np;
            return start_lib_idx(g_libp_order[np]);   // set is loaded; do NOT reload it
        }
        int ni = lib + dir;                      // step within the playing folder (no wrap)
        if (ni < 0 || ni >= g_libp_n) return false;
        return start_track_lib(g_libp_dir, ni);
    }
    if (bi < 0) return false;
    if (calc_step(dir, &bi, &ch, ra)) return start_track(bi, ch);
    return false;
}

static void do_seek(uint32_t ms)
{
    if (!fp || track_size <= 0) return;
    uint32_t dur;
    int sr;
    xSemaphoreTake(st_mux, portMAX_DELAY); dur = g_st.dur_ms; xSemaphoreGive(st_mux);
    sr = cur_sr ? cur_sr : 44100;
    long audio = track_size - track_start;
    long pos = track_start + ((dur > 0) ? (long)((double)ms / dur * audio) : 0);
    if (pos < track_start) pos = track_start;
    if (pos >= track_size) pos = track_size - 1;
    fseek(fp, pos, SEEK_SET);
    in_ptr = in_buf; in_left = 0;
    pos_samples = (uint64_t)ms * sr / 1000;
    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.pos_ms = ms; xSemaphoreGive(st_mux);
}

// Enter STOPPED: release the file, drop the amp, notify the UI. Used for end of
// content, a failed start (missing/corrupt chapter), and a fatal codec error, so the
// UI never sees a phantom PLAYING state with fp==NULL and the amp stuck on.
static void enter_stopped(void)
{
    save_outgoing();     // bank the position (no-op if the track just finished)
    close_track();
    player_status_t s;
    xSemaphoreTake(st_mux, portMAX_DELAY);
    g_st.state = PLAYER_STOPPED;
    s = g_st;
    xSemaphoreGive(st_mux);
    amp(false);
    if (evt_cb) evt_cb(&s);
}

static void handle_cmd(const player_cmd_t *c)
{
    switch (c->type) {
    case CMD_PLAY:
        if (!start_track(c->book_idx, c->chapter)) enter_stopped();
        break;
    case CMD_PLAY_BIY:
        start_track_biy(c->biy_day, c->biy_intro);
        break;
    case CMD_PLAY_LIB: {
        char rel[LIB_PATH_MAX];
        xSemaphoreTake(st_mux, portMAX_DELAY); snprintf(rel, sizeof rel, "%s", pending_lib); xSemaphoreGive(st_mux);
        start_track_lib(rel, c->chapter);       // chapter carries the track index
        break;
    }
    case CMD_PLAY_LIB_SHUF: {
        char rel[LIB_PATH_MAX];
        xSemaphoreTake(st_mux, portMAX_DELAY); snprintf(rel, sizeof rel, "%s", pending_lib); xSemaphoreGive(st_mux);
        if (!start_track_lib_shuffled(rel)) enter_stopped();
        break;
    }
    case CMD_STOP:
        save_outgoing();
        close_track();
        xSemaphoreTake(st_mux, portMAX_DELAY); g_st.state = PLAYER_STOPPED; xSemaphoreGive(st_mux);
        amp(false);
        if (ui_click_pending()) play_click();
        break;
    case CMD_SUSPEND:
        // Like CMD_STOP, but also closes the codec so the ES8311 DAC/bias are off in
        // deep sleep. Runs on the player task so it cannot race esp_codec_dev_write.
        ui_click_cancel();
        save_outgoing();      // last chance to bank the position before deep sleep
        close_track();
        xSemaphoreTake(st_mux, portMAX_DELAY); g_st.state = PLAYER_STOPPED; xSemaphoreGive(st_mux);
        amp(false);
        if (cur_sr) {
            esp_codec_dev_close(playback);
            cur_sr = cur_ch = 0;    // next play reopens via ensure_codec()
            ESP_LOGI(TAG, "codec closed for power-off");
        }
        if (suspend_done) xSemaphoreGive(suspend_done);
        break;
    case CMD_PAUSE_TOGGLE: {
        bool now_playing;
        xSemaphoreTake(st_mux, portMAX_DELAY);
        if (g_st.state == PLAYER_PLAYING) g_st.state = PLAYER_PAUSED;
        else if (g_st.state == PLAYER_PAUSED && fp) g_st.state = PLAYER_PLAYING;
        now_playing = (g_st.state == PLAYER_PLAYING);
        xSemaphoreGive(st_mux);
        amp(now_playing);   // amp follows play/pause
        // CLICK is queued before the button action. If that action pauses the
        // stream, its tick must still play without waiting for a later resume.
        if (!now_playing && ui_click_pending()) play_click();
        break;
    }
    case CMD_NEXT:
    case CMD_PREV:
        // advance() steps a BIBLE chapter or a BIY day per the current track kind.
        // If it tried to start the next track and that failed (missing file), fp is
        // left NULL — stop so the UI never sees PLAYING with no file.
        if (!advance(c->type == CMD_NEXT ? 1 : -1) && !fp)
            enter_stopped();
        break;
    case CMD_SEEK:
        do_seek(c->arg);
        break;
    case CMD_CLICK:
        play_click();
        break;
    case CMD_SET_BASE:
        xSemaphoreTake(st_mux, portMAX_DELAY);
        snprintf(g_base, sizeof g_base, "%s", pending_base);
        xSemaphoreGive(st_mux);
        name_fmt = -1;   // re-detect filename format under the new base
        break;
    case CMD_SET_VOLUME:
        // Applied only while the codec is open; the player loop reconciles the rest.
        if (cur_sr) { esp_codec_dev_set_out_vol(playback, (int)c->arg); vol_applied = (int)c->arg; }
        break;
    }
}

static void player_task(void *arg)
{
    player_cmd_t c;
    for (;;) {
        player_state_t state;
        xSemaphoreTake(st_mux, portMAX_DELAY); state = g_st.state; xSemaphoreGive(st_mux);

        // Reconcile the codec volume with g_st.volume (a CMD_SET_VOLUME received while
        // the codec was closed is not applied by the command handler).
        if (cur_sr) {
            int want;
            xSemaphoreTake(st_mux, portMAX_DELAY); want = g_st.volume; xSemaphoreGive(st_mux);
            if (want != vol_applied) {
                esp_codec_dev_set_out_vol(playback, want);
                vol_applied = want;
                ESP_LOGI(TAG, "volume re-applied: %d", want);
            }
        }

        if (state == PLAYER_PLAYING && fp) {
            // service any pending command without blocking
            if (xQueueReceive(cmd_q, &c, 0) == pdTRUE) { handle_cmd(&c); continue; }
            int r = decode_frame();
            if (r == 1) {                 // natural end of track -> auto next
                close_track();
                ESP_LOGI(TAG, "track end");
                // Reached the end, so tick it and drop its resume point: replaying a
                // finished track should start at the beginning, not at its own end.
                save_progress(true);
                progress_done_pending = true;   // don't let the next start overwrite it
                if (!advance(1))          // no next (end of content / standalone intro)
                    enter_stopped();      // end of content -> stop + notify
            } else if (r < 0) {           // fatal decode/codec error -> stop cleanly
                ESP_LOGE(TAG, "fatal player error -> stop");
                enter_stopped();
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
    suspend_done = xSemaphoreCreateBinary();
    if (!st_mux || !cmd_q || !suspend_done) { ESP_LOGE(TAG, "player state alloc failed"); return ESP_FAIL; }

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

    ui_click_init();

    if (xTaskCreatePinnedToCore(player_task, "player", 6 * 1024, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "player task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_player_play(int book_idx, int chapter)
{
    player_cmd_t c = { .type = CMD_PLAY, .book_idx = book_idx, .chapter = chapter,
                       .biy_day = -1, .biy_intro = -1 };
    post_cmd(&c);
}
void app_player_play_biy(int day)
{
    player_cmd_t c = { .type = CMD_PLAY_BIY, .book_idx = -1, .chapter = 0,
                       .biy_day = day, .biy_intro = -1 };
    post_cmd(&c);
}
void app_player_play_biy_intro(int i)
{
    player_cmd_t c = { .type = CMD_PLAY_BIY, .book_idx = -1, .chapter = 0,
                       .biy_day = -1, .biy_intro = i };
    post_cmd(&c);
}
void app_player_play_lib(const char *rel, int track_idx)
{
    xSemaphoreTake(st_mux, portMAX_DELAY);
    snprintf(pending_lib, sizeof pending_lib, "%s", rel ? rel : "");   // handoff dir to player task
    xSemaphoreGive(st_mux);
    player_cmd_t c = { .type = CMD_PLAY_LIB, .book_idx = -1, .chapter = track_idx,
                       .biy_day = -1, .biy_intro = -1 };
    post_cmd(&c);
}

void app_player_play_lib_shuffled(const char *rel)
{
    xSemaphoreTake(st_mux, portMAX_DELAY);
    snprintf(pending_lib, sizeof pending_lib, "%s", rel ? rel : "");
    xSemaphoreGive(st_mux);
    player_cmd_t c = { .type = CMD_PLAY_LIB_SHUF, .book_idx = -1, .chapter = 0,
                       .biy_day = -1, .biy_intro = -1 };
    post_cmd(&c);
}

// Read-only for the UI's shuffle indicator. Written only by the player task, and a bool
// read is atomic on this core, so no lock is needed for a status glance.
bool app_player_lib_shuffling(void) { return g_libp_shuffle; }
void app_player_toggle_pause(void) { player_cmd_t c = { .type = CMD_PAUSE_TOGGLE }; post_cmd(&c); }
void app_player_stop(void)         { player_cmd_t c = { .type = CMD_STOP };  post_cmd(&c); }

// Stop and close the codec, then block until the player task confirms it. Bounded:
// the caller is on the way into deep sleep and must never be stranded awake, so a
// missed ack costs a warning and a few extra mA, not a hung power-off.
void app_player_suspend(void)
{
    if (!cmd_q || !suspend_done) return;
    xSemaphoreTake(suspend_done, 0);            // clear any stale ack
    player_cmd_t c = { .type = CMD_SUSPEND };
    post_cmd(&c);
    if (xSemaphoreTake(suspend_done, pdMS_TO_TICKS(500)) != pdTRUE)
        ESP_LOGW(TAG, "suspend ack timed out; codec may stay powered");
}
void app_player_next(void)         { player_cmd_t c = { .type = CMD_NEXT };  post_cmd(&c); }
void app_player_prev(void)         { player_cmd_t c = { .type = CMD_PREV };  post_cmd(&c); }
void app_player_seek_ms(uint32_t ms) { player_cmd_t c = { .type = CMD_SEEK, .arg = ms }; post_cmd(&c); }
void app_player_click(void)          { player_cmd_t c = { .type = CMD_CLICK }; post_cmd(&c); }

void app_player_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    // Update the shared value now (so get_volume and a fresh codec-open pick it up), but
    // apply it to the codec on the player task so it can't race an in-flight close/open
    // or read the player-task-local cur_sr from the UI task.
    xSemaphoreTake(st_mux, portMAX_DELAY); g_st.volume = vol; xSemaphoreGive(st_mux);
    player_cmd_t c = { .type = CMD_SET_VOLUME, .arg = (uint32_t)vol };
    post_cmd(&c);
}
int app_player_get_volume(void) { int v; xSemaphoreTake(st_mux, portMAX_DELAY); v = g_st.volume; xSemaphoreGive(st_mux); return v; }
void app_player_set_repeat_all(bool en) { xSemaphoreTake(st_mux, portMAX_DELAY); g_st.repeat_all = en; xSemaphoreGive(st_mux); }
void app_player_set_event_cb(player_event_cb_t cb) { evt_cb = cb; }
void app_player_set_amp_cb(player_amp_cb_t cb) { amp_cb = cb; }
