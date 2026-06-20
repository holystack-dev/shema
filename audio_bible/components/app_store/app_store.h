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
int  app_store_get_theme(int def);     // accent color as 0xRRGGBB
void app_store_set_theme(int hex);
int  app_store_get_screen_timeout(int def);  // screen auto-off seconds, 0 = never
void app_store_set_screen_timeout(int s);
int  app_store_get_poweroff(int def);  // idle power-off seconds, 0 = disabled
void app_store_set_poweroff(int s);
// audio version: SD path of the chosen BIBLE/<LANG>/<VERSION> folder
void app_store_get_version(char *out, int out_sz, const char *def);
void app_store_set_version(const char *path);

// --- resume (last played) ---
bool app_store_get_last(int *book_idx, int *chapter, uint32_t *pos_ms);
void app_store_set_last(int book_idx, int chapter, uint32_t pos_ms);

// --- favourites ---
int  app_store_fav_count(void);
bool app_store_fav_get(int i, track_ref_t *out);
bool app_store_is_fav(int book_idx, int chapter);
void app_store_fav_toggle(int book_idx, int chapter);

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
