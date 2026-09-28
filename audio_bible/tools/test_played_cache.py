#!/usr/bin/env python3
"""Exercise real progress/cache invalidation code against an isolated host card.

Requires a C compiler with AddressSanitizer/UndefinedBehaviorSanitizer. No device,
user card or settings are touched. The small stubs replace only ESP/RTOS services.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="shema-played-") as directory:
    tmp = Path(directory)
    (tmp / "freertos").mkdir()
    for collection in ("BIBLE", "BIY", "LIBRARY"):
        (tmp / collection).mkdir()
    stubs = {
        "esp_log.h": '#define ESP_LOGI(tag, ...) ((void)(tag))\n#define ESP_LOGW ESP_LOGI\n#define ESP_LOGE ESP_LOGI\n',
        "esp_heap_caps.h": '#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define heap_caps_calloc(n,s,c) calloc(n,s)\n#define heap_caps_free free\n',
        "freertos/FreeRTOS.h": '#define portMAX_DELAY 0\n',
        "freertos/semphr.h": 'typedef int SemaphoreHandle_t;\nstatic inline int xSemaphoreCreateMutex(void) { return 1; }\n#define xSemaphoreTake(m,t) ((void)(m))\n#define xSemaphoreGive(m) ((void)(m))\n',
    }
    for name, content in stubs.items():
        (tmp / name).write_text(content)
    (tmp / "test.c").write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "app_played.h"
int main(void) {
    app_played_init();
    uint32_t rev = app_played_lib_revision(), pos = 99;
    assert(rev != 0);
    assert(!app_played_lib_folder_has_done("Songs"));
    app_played_set_lib("Songs/live/one.mp3", 12345, false);
    assert(app_played_lib_revision() == rev);
    assert(!app_played_lib_folder_has_done("Songs"));
    assert(!app_played_get_lib("Songs/live/one.mp3", &pos) && pos == 12345);
    app_played_set_lib("Songs/live/one.mp3", 99999, true);
    assert(app_played_get_lib("Songs/live/one.mp3", &pos) && pos == 0);
    assert(app_played_lib_revision() != rev);
    rev = app_played_lib_revision();
    assert(app_played_lib_folder_has_done(""));
    assert(app_played_lib_folder_has_done("Songs"));
    assert(app_played_lib_folder_has_done("Songs/live"));
    assert(!app_played_lib_folder_has_done("Song"));
    assert(!app_played_lib_folder_has_done("Songs/live2"));
    assert(!app_played_lib_folder_has_done("Songs/live/one.mp3"));
    assert(!app_played_lib_folder_has_done(NULL));
    app_played_set_lib("Songs/live/one.mp3", 0, true);
    app_played_set_lib("Songs/live/one.mp3", 4567, false);
    assert(app_played_lib_revision() == rev);
    assert(app_played_get_lib("Songs/live/one.mp3", &pos) && pos == 4567);
    app_played_set_lib("Songs/two.mp3", 0, true);
    assert(app_played_lib_revision() != rev);
    rev = app_played_lib_revision();
    app_played_clear_all();
    assert(app_played_lib_revision() != rev);
    assert(!app_played_lib_folder_has_done(""));
    assert(!app_played_any());
    // A recycled slot must not inherit the old track's finished flag.
    rev = app_played_lib_revision();
    app_played_set_lib("Prayers/new.mp3", 6789, false);
    assert(!app_played_get_lib("Prayers/new.mp3", &pos) && pos == 6789);
    assert(!app_played_lib_folder_has_done("Prayers"));
    assert(app_played_lib_revision() == rev);
    app_played_set_lib("Prayers/new.mp3", 0, true);
    assert(app_played_lib_folder_has_done("Prayers"));
    assert(app_played_lib_revision() != rev);
    app_played_flush();
    puts("PASS: completion revision, resume stability, folder boundaries, clear/reuse");
}
''')
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-g", f'-DSD_MOUNT="{tmp}"',
               "-I", str(tmp), "-I", str(ROOT / "components/app_played/include"),
               "-I", str(ROOT / "components/bible_data/include"),
               str(tmp / "test.c"), str(ROOT / "components/app_played/app_played.c"),
               str(ROOT / "components/bible_data/bible_data.c"), "-o", str(tmp / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(tmp / "test")], check=True)
