#include "ui_click.h"

#include <math.h>
#include <limits.h>

#define CLICK_RATE 48000
#define CLICK_FRAMES 480
static int16_t wave[CLICK_FRAMES];
static uint32_t phase;
static bool pending;

void ui_click_init(void)
{
    for (int i = 0; i < CLICK_FRAMES; i++) {
        float envelope = expf(-(float)i / (CLICK_FRAMES * 0.22f));
        wave[i] = (int16_t)(5000.0f * envelope *
                           sinf(2.0f * 3.14159265359f * 2000.0f * i / CLICK_RATE));
    }
    pending = false;
}

void ui_click_start(void) { phase = 0; pending = true; }
void ui_click_cancel(void) { pending = false; }
bool ui_click_pending(void) { return pending; }

void ui_click_mix(int16_t *stereo, int frames, int sample_rate)
{
    if (!pending || sample_rate <= 0) return;
    const uint32_t end = CLICK_FRAMES * (uint32_t)sample_rate;
    for (int i = 0; i < frames && phase < end; i++, phase += CLICK_RATE) {
        int tick = wave[phase / sample_rate];
        for (int ch = 0; ch < 2; ch++) {
            int mixed = stereo[2 * i + ch] + tick;
            if (mixed > INT16_MAX) mixed = INT16_MAX;
            if (mixed < INT16_MIN) mixed = INT16_MIN;
            stereo[2 * i + ch] = (int16_t)mixed;
        }
    }
    if (phase >= end) pending = false;
}
