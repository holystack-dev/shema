// app_played — per-track progress, stored on the SD card (NVS is too small for
// per-track Library entries, and progress moves with the card).
//
// Three files, one per collection, at the root of the folder they describe:
//   /sdcard/BIBLE/played.txt   /sdcard/BIY/played.txt   /sdcard/LIBRARY/played.txt
// They are invisible to the browser: the Library scan only accepts names ending
// ".mp3", BIY only "Day "/"Intro " prefixes, and Bible builds its paths directly.
//
// Per track:
//   pos_ms  — resume position
//   done    — played to the end at least once
// Finishing a track sets done and clears pos_ms.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load all three files into RAM. Call once, after the SD card is mounted and after
// bible_data is available. Safe to call with no card / no files: everything simply
// reads back as unplayed.
void app_played_init(void);

// --- query: returns true if the track was ever finished; *pos_ms (may be NULL) gets
// the saved resume offset, 0 when there is none. ---
bool app_played_get_bible(int book_idx, int chapter, uint32_t *pos_ms);
bool app_played_get_biy(int day, int intro, uint32_t *pos_ms);
bool app_played_get_lib(const char *rel, uint32_t *pos_ms);   // rel = path under LIBRARY/
// In-memory completion hints for the folder browser. Revision changes only when
// completion changes or history is cleared, not on resume-position updates.
uint32_t app_played_lib_revision(void);
bool app_played_lib_folder_has_done(const char *rel);

// --- update: records in RAM and appends the line to the card. ---
// `done` true means the track reached its natural end (pos_ms is ignored and stored
// as 0). `done` false records a mid-track position to resume from.
void app_played_set_bible(int book_idx, int chapter, uint32_t pos_ms, bool done);
void app_played_set_biy(int day, int intro, uint32_t pos_ms, bool done);
void app_played_set_lib(const char *rel, uint32_t pos_ms, bool done);

// Compact the progress files if they have grown. Called before unmount on the way into
// deep sleep.
void app_played_flush(void);

// Forget everything, on the card as well as in RAM (Settings -> Clear played history).
void app_played_clear_all(void);

// True once any progress at all is recorded — lets Settings grey out "Clear" when
// there is nothing to clear.
bool app_played_any(void);

#ifdef __cplusplus
}
#endif
