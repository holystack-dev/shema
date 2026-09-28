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

// What the current track is: a Bible book/chapter, a Bible-in-a-Year episode,
// or a Library track (generic LIBRARY/<category>/<file>.mp3).
typedef enum { TRACK_BIBLE = 0, TRACK_BIY = 1, TRACK_LIB = 2 } track_kind_t;

typedef struct {
    player_state_t state;
    track_kind_t kind;   // selects which identity fields below are valid
    int  book_idx;    // BIBLE: 0..72 (index into BIBLE_BOOKS), -1 if none
    int  chapter;     // BIBLE: 0..chapter_count (0 = book intro)
    int  biy_day;     // BIY: 1..365, or -1 (e.g. an intro)
    int  biy_intro;   // BIY: 0-based intro index, or -1 (a day)
    int  lib_idx;     // LIBRARY: track index within the playing folder, or -1
    uint32_t pos_ms;  // current playback position
    uint32_t dur_ms;  // track duration (computed from the MP3 at play time)
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

void app_player_play(int book_idx, int chapter); // BIBLE: start/replace track (chapter 0 = intro)
void app_player_toggle_pause(void);
void app_player_stop(void);
// Stop AND close the codec device (powers the ES8311 down). Blocks until the
// player task acknowledges, up to 500 ms. For the deep-sleep power-off path.
void app_player_suspend(void);
void app_player_next(void);
void app_player_prev(void);
void app_player_seek_ms(uint32_t ms);
void app_player_set_volume(int vol);             // 0..100
int  app_player_get_volume(void);
void app_player_set_repeat_all(bool enable);
void app_player_get_status(player_status_t *out);
void app_player_set_event_cb(player_event_cb_t cb);
void app_player_set_amp_cb(player_amp_cb_t cb);

// Play a short UI tick, mixed into active audio or rendered at the current
// codec rate when paused/stopped. Does not advance the saved track position.
void app_player_click(void);

// --- audio version selection: BIBLE/<LANG>/<VERSION> folders on the SD card ---
int         app_player_versions(void);        // scan card once, returns count
const char *app_player_version_name(int i);   // UI label, e.g. "POC 1 (ML)"
const char *app_player_version_short(int i);  // short name, e.g. "POC 1"
const char *app_player_version_dir(int i);    // SD path of version i
int         app_player_find_version(const char *dir);  // index of dir, or -1
void        app_player_set_base(const char *dir);      // point playback at a version folder

// --- Bible in a Year: BIY/<EN|ML>/ "Day NNN - title.mp3" + "Intro NN - title.mp3" ---
int         app_player_biy_langs(void);          // number of BIY/<lang> folders with episodes
const char *app_player_biy_lang_name(int i);     // folder name = on-screen label (e.g. "ML")
int         app_player_biy_find_lang(const char *name); // index of a language by folder name, or -1
int         app_player_biy_set_lang(int i);      // select language by index; (re)scans, returns days
const char *app_player_biy_day_title(int day);   // title for 1..365 ("" if absent)
int         app_player_biy_intro_count(void);
const char *app_player_biy_intro_title(int i);   // 0-based
void        app_player_play_biy(int day);        // 1..365
void        app_player_play_biy_intro(int i);    // 0-based

// --- Library: LIBRARY/<folder>/.../<track>.mp3 — generic nested folder browser ---
bool        app_player_has_library(void);          // true if /sdcard/LIBRARY exists
// Browse a folder (rel path under LIBRARY, "" = root). Lists sub-folders then *.mp3
// tracks (each sorted). Populates the browse context; returns the entry count.
int         app_player_lib_browse(const char *rel);
int         app_player_lib_count(void);            // entries in the current browse listing
const char *app_player_lib_name(int i);            // entry display name (folder or track, no ".mp3")
// Browsed track's path relative to LIBRARY/ — the key app_played uses. False for folders.
bool app_player_lib_rel(int i, char *out, size_t out_sz);
// True when browsed entry `i` is a folder and every track under it has been finished.
// Walks the card, so it is bounded: a very large folder reports false rather than stall.
bool app_player_lib_folder_done(int i);
bool        app_player_lib_is_dir(int i);          // true = sub-folder, false = playable track
int         app_player_lib_track_of(int i);        // file entry -> its track index, or -1 for a folder
// Play a track by its track index within folder `rel`; auto-advances through that folder.
// Also cancels shuffle.
void        app_player_play_lib(const char *rel, int track_idx);
// Shuffle everything under `rel` INCLUDING its sub-folders, and start playing. Library
// only — Bible and Bible-in-a-Year are meant to be heard in sequence.
void        app_player_play_lib_shuffled(const char *rel);
bool        app_player_lib_shuffling(void);        // true while a shuffle set is playing
const char *app_player_lib_playing_name(void);     // current Library track name (display), or ""
const char *app_player_lib_playing_folder(void);   // current Library track's folder (last path component)
const char *app_player_lib_playing_dir(void);      // current Library track's full folder rel path (for resume)

#ifdef __cplusplus
}
#endif
