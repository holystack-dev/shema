// app_store — NVS persistence: settings, resume position, favourites, playlists.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_MAX_PLAYLISTS 8
#define STORE_MAX_TRACKS    128
#define STORE_NAME_LEN      24
// ~190 B per entry (inline Library dir+name): 40 entries ≈ 7.5 KB RAM and NVS.
// Rewritten only when the front entry changes.
#define STORE_MAX_HIST      40

// Library items are identified by folder rel-path + file name, which stay valid if the
// card's contents change.
#define STORE_MAX_FAV_BIY   64
#define STORE_MAX_FAV_LIB   32
#define STORE_LIB_DIR_LEN   96
#define STORE_LIB_NAME_LEN  80

typedef struct {
    uint8_t book_idx;   // 0..72
    uint8_t chapter;    // 1..N
} track_ref_t;

void app_store_init(void);

// --- settings ---
int  app_store_get_volume(int def);
void app_store_set_volume(int v);
int  app_store_get_brightness(int def);
void app_store_set_brightness(int v);
bool app_store_get_repeat_all(bool def);
void app_store_set_repeat_all(bool v);
// Swipe-right-to-go-back (optional; the Back button is always available).
bool app_store_get_swipe_back(bool def);
void app_store_set_swipe_back(bool v);
int  app_store_get_theme(int def);     // accent color as 0xRRGGBB
void app_store_set_theme(int hex);
int  app_store_get_screen_timeout(int def);  // screen auto-off seconds, 0 = never
void app_store_set_screen_timeout(int s);
int  app_store_get_poweroff(int def);  // idle power-off seconds, 0 = disabled
void app_store_set_poweroff(int s);

// --- audio version: SD path of the chosen BIBLE/<LANG>/<VERSION> folder ---
void app_store_get_version(char *out, int out_sz, const char *def);
void app_store_set_version(const char *path);

// --- Bible-in-a-Year language: SD folder name of the chosen BIY/<lang> ---
void app_store_get_biy_lang(char *out, int out_sz, const char *def);
void app_store_set_biy_lang(const char *name);

// --- resume (last played) ---
// kind: 0 = BIBLE (book/chapter), 1 = BIY (day/intro), 2 = LIBRARY, -1 = nothing saved
int  app_store_get_last_kind(void);
bool app_store_get_last(int *book_idx, int *chapter, uint32_t *pos_ms);   // BIBLE only
void app_store_set_last(int book_idx, int chapter, uint32_t pos_ms);
bool app_store_get_last_biy(int *day, int *intro, uint32_t *pos_ms);      // BIY only
void app_store_set_last_biy(int day, int intro, uint32_t pos_ms);
bool app_store_get_last_lib(char *dir, int dir_sz, int *idx, char *name, int name_sz, uint32_t *pos_ms);  // LIBRARY only
void app_store_set_last_lib(const char *dir, int idx, const char *name, uint32_t pos_ms);

// --- recent history (MRU, newest first; all three collections) ---
// kind 0 = BIBLE (a=book_idx, b=chapter); 1 = BIY (a=day, b=intro, one is -1);
// 2 = LIBRARY (b=track index, dir/name carry the identity).
int  app_store_hist_count(void);
bool app_store_hist_get(int i, int *kind, int *a, int *b, uint32_t *pos_ms);
// Library variant of hist_get; dir/name may be NULL if not wanted. Returns false for
// a non-Library entry, so callers can branch on it directly.
bool app_store_hist_get_lib(int i, char *dir, int dir_sz, char *name, int name_sz, int *track);
void app_store_hist_push(int kind, int a, int b, uint32_t pos_ms);  // dedupes + moves to front
void app_store_hist_push_lib(const char *dir, const char *name, int track, uint32_t pos_ms);

// --- favourites: three independent layers ---
// Each collection is keyed differently, so they are kept as separate lists rather than
// one tagged list: a Bible ref is 2 bytes, a Library ref needs ~180.
int  app_store_fav_count(void);                     // BIBLE
bool app_store_fav_get(int i, track_ref_t *out);
bool app_store_is_fav(int book_idx, int chapter);
void app_store_fav_toggle(int book_idx, int chapter);

int  app_store_favbiy_count(void);                  // BIBLE IN A YEAR
bool app_store_favbiy_get(int i, int *day, int *intro);
bool app_store_is_favbiy(int day, int intro);
void app_store_favbiy_toggle(int day, int intro);
void app_store_favbiy_remove(int i);

int  app_store_favlib_count(void);                  // LIBRARY
bool app_store_favlib_get(int i, char *dir, int dir_sz, char *name, int name_sz, int *track);
bool app_store_is_favlib(const char *dir, const char *name);
void app_store_favlib_toggle(const char *dir, const char *name, int track);
void app_store_favlib_remove(int i);

// --- playlists (0..count-1) ---
int  app_store_pl_count(void);
bool app_store_pl_name(int pl, char *out, int out_sz);
int  app_store_pl_create(const char *name);          // returns index or -1
void app_store_pl_delete(int pl);
int  app_store_pl_track_count(int pl);
bool app_store_pl_track_get(int pl, int i, track_ref_t *out);
void app_store_pl_add(int pl, int book_idx, int chapter);
void app_store_pl_remove(int pl, int i);

#ifdef __cplusplus
}
#endif
