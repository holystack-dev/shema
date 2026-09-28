// Native check: cc -Wall -Wextra -Werror -fsanitize=address,undefined \
//   -Icomponents/app_player tools/test_ui_click.c components/app_player/ui_click.c \
//   -lm -o /tmp/test-ui-click && /tmp/test-ui-click
#include "ui_click.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const int rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    int16_t whole[1024], chunks[1024], loud[1024];
    ui_click_init();
    assert(!ui_click_pending());
    for (unsigned r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        int frames = (rates[r] + 99) / 100;
        memset(whole, 0, sizeof whole);
        ui_click_start();
        ui_click_mix(whole, frames - 1, rates[r]);
        assert(ui_click_pending());
        ui_click_mix(whole + 2 * (frames - 1), 1, rates[r]);
        assert(!ui_click_pending());
        int energy = 0;
        for (int i = 0; i < 512; i++) {
            assert(whole[2*i] == whole[2*i+1]);
            if (i >= frames) assert(whole[2*i] == 0);
            energy += whole[2*i] != 0;
        }
        assert(energy >= frames / 3);   // 8 kHz samples hit a 2 kHz zero every other sample
        memset(chunks, 0, sizeof chunks);
        ui_click_start();
        for (int i = 0; i < 512; i += 16) ui_click_mix(chunks + 2*i, 16, rates[r]);
        assert(memcmp(whole, chunks, sizeof whole) == 0);

        // Real full-scale content must saturate, never wrap to the opposite sign.
        for (int i = 0; i < 512; i++) { loud[2*i] = INT16_MAX; loud[2*i+1] = INT16_MIN; }
        ui_click_start();
        ui_click_mix(loud, 512, rates[r]);
        for (int i = 0; i < 512; i++) {
            int tick = whole[2*i];
            assert(loud[2*i] == (tick > 0 ? INT16_MAX : INT16_MAX + tick));
            assert(loud[2*i+1] == (tick < 0 ? INT16_MIN : INT16_MIN + tick));
        }
    }
    memset(whole, 0, sizeof whole);
    ui_click_start();
    ui_click_cancel();
    ui_click_mix(whole, 512, 44100);
    for (int i = 0; i < 1024; i++) assert(whole[i] == 0);
    puts("PASS: 9 MP3 sample rates; 10 ms duration; stereo; chunk boundaries; saturation; cancellation");
}
