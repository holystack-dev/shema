#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_played.h"
#include "bible_data.h"

static const char *TAG = "played";

#ifndef SD_MOUNT
#define SD_MOUNT   "/sdcard"
#endif
#define F_BIBLE    SD_MOUNT "/BIBLE/played.txt"
#define F_BIY      SD_MOUNT "/BIY/played.txt"
#define F_LIB      SD_MOUNT "/LIBRARY/played.txt"

// Mirrors app_player's limits; a Library entry is a path relative to LIBRARY/.
#define LIB_PATH_MAX   192
#define LIB_MAX_TRACK  1500
#define BIY_DAY_MAX    365
#define BIY_MAX_INTRO  24
#define BIY_SLOTS      (BIY_DAY_MAX + BIY_MAX_INTRO)

// Compact a file once it holds more than this many lines per distinct track.
#define COMPACT_RATIO  4

typedef struct {
    uint32_t pos_ms;
    uint8_t  done;
    uint8_t  used;      // distinguishes "recorded at 0 ms" from "never seen"
} slot_t;

typedef struct {
    char     rel[LIB_PATH_MAX];
    uint32_t pos_ms;
    uint8_t  done;
} lib_slot_t;

// BIBLE is indexed by a flat offset table rather than [book][chapter]: chapter counts
// vary from 1 to 150, so a rectangular array would waste most of its space. Slot 0 of
// each book is the book intro (chapter 0), matching the player's numbering.
static uint16_t  bible_off[BIBLE_BOOK_COUNT + 1];
static slot_t   *bible_slot;                 // [bible_off[BIBLE_BOOK_COUNT]]
static slot_t   *biy_slot;                   // [BIY_SLOTS]
static lib_slot_t *lib_slot;                 // [LIB_MAX_TRACK], PSRAM
static int        lib_n;
static uint32_t   lib_done_revision = 1;

static SemaphoreHandle_t mux;
static bool inited;
// Counted per file so compaction can be decided without walking the file again.
static int lines_bible, lines_biy, lines_lib;
static int dirty_bible, dirty_biy, dirty_lib;

static void lock(void)   { if (mux) xSemaphoreTake(mux, portMAX_DELAY); }
static void unlock(void) { if (mux) xSemaphoreGive(mux); }

// ---- BIY slot mapping: days 1..365 -> 0..364, intros 0..23 -> 365..388 ----
// The player passes day = -1 for an intro and intro = -1 for a day, never both.
static int biy_index(int day, int intro)
{
    if (day >= 1 && day <= BIY_DAY_MAX) return day - 1;
    if (intro >= 0 && intro < BIY_MAX_INTRO) return BIY_DAY_MAX + intro;
    return -1;
}

static int bible_index(int book_idx, int chapter)
{
    if (book_idx < 0 || book_idx >= BIBLE_BOOK_COUNT) return -1;
    if (chapter < 0 || chapter > BIBLE_BOOKS[book_idx].chapter_count) return -1;
    return bible_off[book_idx] + chapter;
}

static int lib_find(const char *rel)
{
    for (int i = 0; i < lib_n; i++)
        if (strcmp(lib_slot[i].rel, rel) == 0) return i;
    return -1;
}

// ---- loading ----
//
// Append-only: each line is a complete record and a later line for the same track
// supersedes an earlier one. A torn final line fails to parse and is dropped.

static void load_bible(void)
{
    FILE *f = fopen(F_BIBLE, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int b, c; unsigned long p; int d;
        if (sscanf(line, "B %d %d %lu %d", &b, &c, &p, &d) != 4) continue;
        int i = bible_index(b, c);
        if (i < 0) continue;                       // book/chapter no longer exists
        bible_slot[i].pos_ms = (uint32_t)p;
        bible_slot[i].done   = d ? 1 : 0;
        bible_slot[i].used   = 1;
        lines_bible++;
    }
    fclose(f);
}

static void load_biy(void)
{
    FILE *f = fopen(F_BIY, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        int day, intro; unsigned long p; int d;
        if (sscanf(line, "Y %d %d %lu %d", &day, &intro, &p, &d) != 4) continue;
        int i = biy_index(day, intro);
        if (i < 0) continue;
        biy_slot[i].pos_ms = (uint32_t)p;
        biy_slot[i].done   = d ? 1 : 0;
        biy_slot[i].used   = 1;
        lines_biy++;
    }
    fclose(f);
}

static void load_lib(void)
{
    FILE *f = fopen(F_LIB, "r");
    if (!f) return;
    // The path is last on the line precisely because it may contain spaces — every
    // fixed-width field is parsed off the front first, then the remainder is the path.
    char line[LIB_PATH_MAX + 64];
    while (fgets(line, sizeof line, f)) {
        unsigned long p; int d; int used = 0;
        if (sscanf(line, "L %lu %d %n", &p, &d, &used) < 2 || used <= 0) continue;
        char *rel = line + used;
        char *nl = strchr(rel, '\n');
        if (!nl) continue;                          // torn final line — drop it
        *nl = 0;
        if (!rel[0] || strlen(rel) >= LIB_PATH_MAX) continue;
        lines_lib++;
        int i = lib_find(rel);
        if (i < 0) {
            if (lib_n >= LIB_MAX_TRACK) continue;
            i = lib_n++;
            snprintf(lib_slot[i].rel, LIB_PATH_MAX, "%s", rel);
        }
        lib_slot[i].pos_ms = (uint32_t)p;
        lib_slot[i].done   = d ? 1 : 0;
    }
    fclose(f);
}

// ---- compaction: rewrite a file holding only the surviving state ----
// Written to a temp file and renamed, so an interruption leaves the old file intact
// rather than a half-written one.
static void compact_bible(void)
{
    FILE *f = fopen(F_BIBLE ".tmp", "w");
    if (!f) return;
    int n = 0;
    for (int b = 0; b < BIBLE_BOOK_COUNT; b++)
        for (int c = 0; c <= BIBLE_BOOKS[b].chapter_count; c++) {
            slot_t *s = &bible_slot[bible_off[b] + c];
            if (!s->used) continue;
            fprintf(f, "B %d %d %lu %d\n", b, c, (unsigned long)s->pos_ms, s->done);
            n++;
        }
    fclose(f);
    remove(F_BIBLE);
    rename(F_BIBLE ".tmp", F_BIBLE);
    lines_bible = n;
}

static void compact_biy(void)
{
    FILE *f = fopen(F_BIY ".tmp", "w");
    if (!f) return;
    int n = 0;
    for (int i = 0; i < BIY_SLOTS; i++) {
        if (!biy_slot[i].used) continue;
        int day   = (i < BIY_DAY_MAX) ? i + 1 : -1;
        int intro = (i < BIY_DAY_MAX) ? -1 : i - BIY_DAY_MAX;
        fprintf(f, "Y %d %d %lu %d\n", day, intro, (unsigned long)biy_slot[i].pos_ms, biy_slot[i].done);
        n++;
    }
    fclose(f);
    remove(F_BIY);
    rename(F_BIY ".tmp", F_BIY);
    lines_biy = n;
}

static void compact_lib(void)
{
    FILE *f = fopen(F_LIB ".tmp", "w");
    if (!f) return;
    for (int i = 0; i < lib_n; i++)
        fprintf(f, "L %lu %d %s\n", (unsigned long)lib_slot[i].pos_ms, lib_slot[i].done, lib_slot[i].rel);
    fclose(f);
    remove(F_LIB);
    rename(F_LIB ".tmp", F_LIB);
    lines_lib = lib_n;
}

static int bible_used(void)
{
    int n = 0;
    for (int i = 0; i < bible_off[BIBLE_BOOK_COUNT]; i++) if (bible_slot[i].used) n++;
    return n;
}
static int biy_used(void)
{
    int n = 0;
    for (int i = 0; i < BIY_SLOTS; i++) if (biy_slot[i].used) n++;
    return n;
}

void app_played_init(void)
{
    if (inited) return;

    for (int b = 0; b < BIBLE_BOOK_COUNT; b++)
        bible_off[b + 1] = bible_off[b] + BIBLE_BOOKS[b].chapter_count + 1;   // +1 = intro

    bible_slot = calloc(bible_off[BIBLE_BOOK_COUNT], sizeof(slot_t));
    biy_slot   = calloc(BIY_SLOTS, sizeof(slot_t));
    // ~290 KB — far too big for internal RAM, and PSRAM is where the player already
    // keeps its own track tables.
    lib_slot   = heap_caps_calloc(LIB_MAX_TRACK, sizeof(lib_slot_t), MALLOC_CAP_SPIRAM);
    if (!bible_slot || !biy_slot || !lib_slot) {
        ESP_LOGE(TAG, "alloc failed — progress tracking disabled");
        free(bible_slot); free(biy_slot); heap_caps_free(lib_slot);
        bible_slot = biy_slot = NULL; lib_slot = NULL;
        return;
    }
    mux = xSemaphoreCreateMutex();
    inited = true;

    load_bible();
    load_biy();
    load_lib();

    // Fold away the accumulated history now, while nothing is playing — never mid-track,
    // where a full rewrite could stall an SD read long enough to glitch the audio.
    int ub = bible_used(), uy = biy_used();
    if (ub && lines_bible > ub * COMPACT_RATIO) compact_bible();
    if (uy && lines_biy   > uy * COMPACT_RATIO) compact_biy();
    if (lib_n && lines_lib > lib_n * COMPACT_RATIO) compact_lib();

    ESP_LOGI(TAG, "progress loaded: bible=%d biy=%d library=%d", ub, uy, lib_n);
}

// ---- queries ----
bool app_played_get_bible(int book_idx, int chapter, uint32_t *pos_ms)
{
    if (pos_ms) *pos_ms = 0;
    if (!inited) return false;
    int i = bible_index(book_idx, chapter);
    if (i < 0) return false;
    lock();
    if (pos_ms) *pos_ms = bible_slot[i].pos_ms;
    bool d = bible_slot[i].done;
    unlock();
    return d;
}

bool app_played_get_biy(int day, int intro, uint32_t *pos_ms)
{
    if (pos_ms) *pos_ms = 0;
    if (!inited) return false;
    int i = biy_index(day, intro);
    if (i < 0) return false;
    lock();
    if (pos_ms) *pos_ms = biy_slot[i].pos_ms;
    bool d = biy_slot[i].done;
    unlock();
    return d;
}

bool app_played_get_lib(const char *rel, uint32_t *pos_ms)
{
    if (pos_ms) *pos_ms = 0;
    if (!inited || !rel || !rel[0]) return false;
    lock();
    int i = lib_find(rel);
    bool d = false;
    if (i >= 0) { if (pos_ms) *pos_ms = lib_slot[i].pos_ms; d = lib_slot[i].done; }
    unlock();
    return d;
}

uint32_t app_played_lib_revision(void)
{
    lock();
    uint32_t revision = lib_done_revision;
    unlock();
    return revision;
}

bool app_played_lib_folder_has_done(const char *rel)
{
    if (!inited || !rel) return false;
    size_t len = strlen(rel);
    bool found = false;
    lock();
    for (int i = 0; i < lib_n; i++) {
        if (lib_slot[i].done && (!len ||
            (strncmp(lib_slot[i].rel, rel, len) == 0 && lib_slot[i].rel[len] == '/'))) {
            found = true;
            break;
        }
    }
    unlock();
    return found;
}

// ---- updates ----
//
// Each setter appends one line (open/append/close). Callers update at track boundaries,
// not on the 5 s position tick.

static void append(const char *path, const char *line)
{
    FILE *f = fopen(path, "a");
    if (!f) { ESP_LOGW(TAG, "cannot append to %s", path); return; }
    fputs(line, f);
    fclose(f);
}

void app_played_set_bible(int book_idx, int chapter, uint32_t pos_ms, bool done)
{
    if (!inited) return;
    int i = bible_index(book_idx, chapter);
    if (i < 0) return;
    if (done) pos_ms = 0;
    lock();
    bible_slot[i].pos_ms = pos_ms;
    bible_slot[i].done  |= done ? 1 : 0;      // finishing once is permanent
    bible_slot[i].used   = 1;
    int d = bible_slot[i].done;
    lines_bible++; dirty_bible = 1;
    unlock();
    char line[64];
    snprintf(line, sizeof line, "B %d %d %lu %d\n", book_idx, chapter, (unsigned long)pos_ms, d);
    append(F_BIBLE, line);
}

void app_played_set_biy(int day, int intro, uint32_t pos_ms, bool done)
{
    if (!inited) return;
    int i = biy_index(day, intro);
    if (i < 0) return;
    if (done) pos_ms = 0;
    lock();
    biy_slot[i].pos_ms = pos_ms;
    biy_slot[i].done  |= done ? 1 : 0;
    biy_slot[i].used   = 1;
    int d = biy_slot[i].done;
    lines_biy++; dirty_biy = 1;
    unlock();
    char line[64];
    snprintf(line, sizeof line, "Y %d %d %lu %d\n", day, intro, (unsigned long)pos_ms, d);
    append(F_BIY, line);
}

void app_played_set_lib(const char *rel, uint32_t pos_ms, bool done)
{
    if (!inited || !rel || !rel[0] || strlen(rel) >= LIB_PATH_MAX) return;
    if (done) pos_ms = 0;
    lock();
    int i = lib_find(rel);
    if (i < 0) {
        if (lib_n >= LIB_MAX_TRACK) { unlock(); return; }
        i = lib_n++;
        memset(&lib_slot[i], 0, sizeof lib_slot[i]); // a slot may be reused after Clear
        snprintf(lib_slot[i].rel, LIB_PATH_MAX, "%s", rel);
    }
    lib_slot[i].pos_ms = pos_ms;
    if (done && !lib_slot[i].done && ++lib_done_revision == 0) lib_done_revision = 1;
    lib_slot[i].done  |= done ? 1 : 0;
    int d = lib_slot[i].done;
    lines_lib++; dirty_lib = 1;
    unlock();
    char line[LIB_PATH_MAX + 64];
    snprintf(line, sizeof line, "L %lu %d %s\n", (unsigned long)pos_ms, d, rel);
    append(F_LIB, line);
}

void app_played_flush(void)
{
    if (!inited) return;
    // Appends are already closed; compact the files if they have grown.
    lock();
    int ub = bible_used(), uy = biy_used();
    bool cb = dirty_bible && ub && lines_bible > ub * COMPACT_RATIO;
    bool cy = dirty_biy   && uy && lines_biy   > uy * COMPACT_RATIO;
    bool cl = dirty_lib   && lib_n && lines_lib > lib_n * COMPACT_RATIO;
    dirty_bible = dirty_biy = dirty_lib = 0;
    if (cb) compact_bible();
    if (cy) compact_biy();
    if (cl) compact_lib();
    unlock();
}

void app_played_clear_all(void)
{
    if (!inited) return;
    lock();
    memset(bible_slot, 0, (size_t)bible_off[BIBLE_BOOK_COUNT] * sizeof(slot_t));
    memset(biy_slot, 0, (size_t)BIY_SLOTS * sizeof(slot_t));
    lib_n = 0;
    if (++lib_done_revision == 0) lib_done_revision = 1;
    lines_bible = lines_biy = lines_lib = 0;
    dirty_bible = dirty_biy = dirty_lib = 0;
    remove(F_BIBLE);
    remove(F_BIY);
    remove(F_LIB);
    unlock();
    ESP_LOGW(TAG, "played history cleared");
}

bool app_played_any(void)
{
    if (!inited) return false;
    lock();
    bool any = (lib_n > 0) || bible_used() || biy_used();
    unlock();
    return any;
}
