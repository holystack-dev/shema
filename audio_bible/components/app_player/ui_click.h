#pragma once

#include <stdbool.h>
#include <stdint.h>

// Player-task-only 10 ms tap sound. Mixing never inserts track samples or
// changes the playback position. Standalone ticks use a zeroed stereo buffer.
void ui_click_init(void);
void ui_click_start(void);
void ui_click_cancel(void);
bool ui_click_pending(void);
void ui_click_mix(int16_t *stereo, int frames, int sample_rate);
