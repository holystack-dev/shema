// app_player — MP3 (Helix) -> ES8311 (esp_codec_dev) audio player.
// Thread-safe control API: all commands are queued to the player task.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_state_t;

typedef struct {
    player_state_t state;
    int  book_idx;    // 0..72 (index into BIBLE_BOOKS), -1 if none
    int  chapter;     // 1..chapter_count
    uint32_t pos_ms;  // current playback position
    uint32_t dur_ms;  // track duration (from baked metadata)
    int  volume;      // 0..100
    bool repeat_all;
} player_status_t;

// Called from the player task when a track finishes naturally (EOF) and there
// is no auto-next (e.g. end of Revelation with repeat-all off). UI may react.
typedef void (*player_event_cb_t)(const player_status_t *st);

// Called when playback starts/stops so the board can gate the speaker amp.
// Invoked with true before the first audio is written, false once idle.
typedef void (*player_amp_cb_t)(bool on);

esp_err_t app_player_init(void);

// --- audio version selection: BIBLE/<LANG>/<VERSION> folders on the SD card ---
int         app_player_versions(void);        // scan card once, returns count
const char *app_player_version_name(int i);   // UI label, e.g. "ML  POC Dramatized"
const char *app_player_version_short(int i);  // short name, e.g. "POC Dramatized"
const char *app_player_version_dir(int i);    // SD path of version i
int         app_player_find_version(const char *dir);  // index of dir, or -1
void        app_player_set_base(const char *dir);      // point playback at a version folder

void app_player_play(int book_idx, int chapter); // start/replace current track
void app_player_toggle_pause(void);
void app_player_stop(void);
void app_player_next(void);
void app_player_prev(void);
void app_player_seek_ms(uint32_t ms);
void app_player_set_volume(int vol);             // 0..100
int  app_player_get_volume(void);
void app_player_set_repeat_all(bool enable);
void app_player_get_status(player_status_t *out);
void app_player_set_event_cb(player_event_cb_t cb);
void app_player_set_amp_cb(player_amp_cb_t cb);

// Play a short UI click/tick (ignored while a chapter is playing, so audio is
// never interrupted). Wire to UI tap feedback.
void app_player_click(void);

#ifdef __cplusplus
}
#endif
