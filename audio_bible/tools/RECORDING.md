# Device navigation video

The [published video package](../../design/product-videos/2026-09-26/README.md)
contains the tour, poster, chapter preview and action timings.

The source is the connected device's actual 360×360 LCD framebuffer. This is a
screen recording, without a camera view, finger overlay or recorded audio.
[`record_device.py`](record_device.py) captures it and exports a website-ready
H.264 MP4, PNG poster and JSON chapter/action timings. Device timestamps are
preserved; the 30fps file repeats existing frames between display updates and
does not invent intermediate motion.

## Tour

[`navigation_tour.json`](navigation_tour.json) is the editable shot plan:

1. Home, Bible books, chapter grid and New Testament.
2. Bible in a Year, daily episodes and introductions.
3. Library, nested episode folders, the large Songs collection, alphabet picker
   and Back.
4. Favourites, Edit, removal confirmation and Cancel.
5. A saved passage, Play/Pause, filled favourite icon, sleep choices and active moon.
6. The other saved collections and Recent listening.
7. Settings: versions, switches, timeout, power-off, colours, clear confirmation
   and Cancel, then Home.

The tour is separate from the regression test sequence. It uses readable
pauses, real pointer taps and LVGL's own animated scrolling. It never confirms a
deletion or changes persistent Settings. Playing the saved passage
updates Recent/resume state. The demonstrated sleep timer is turned off again.

## One command

From the repository root, with the device connected and serial monitors closed
(`PORT` is the board's serial port, e.g. `/dev/cu.usbmodem101` or `/dev/ttyACM0`):

```sh
bash audio_bible/tools/review_device.sh PORT \
  screenshots-out/video-YYYY-MM-DD-HHMM \
  --video audio_bible/tools/navigation_tour.json
```

This shares the screenshot workflow's ESP-IDF setup and exit trap: build/flash
capture firmware → hardware checks → record/export → rebuild/flash normal
firmware, including on failure or interrupt. Requires `ffmpeg` on PATH and
ESP-IDF v5.3.2 (which supplies Python/pyserial). Set `IDF_EXPORT_SCRIPT` if its
location differs from `$HOME/esp/esp-idf-v5.3.2/export.sh`.

Use a fresh output directory. Results live in `product-video/`; original
compressed frames, device timestamps and the serial log remain in `recording/`.
Copy the MP4, PNG and JSON to a dated folder under `design/product-videos/` for
website use. Keep the original 360px resolution; display with a CSS circle mask.
Check `release.log` ends with `Hash of data verified` and `Done`.

## Re-record or re-export without repeating setup

If capture firmware is already installed:

```sh
source $HOME/esp/esp-idf-v5.3.2/export.sh
python audio_bible/tools/record_device.py PORT \
  audio_bible/tools/navigation_tour.json screenshots-out/new-recording \
  design/product-videos/new-date/navigation-tour.mp4
```

To re-export saved source frames without connecting or flashing the device:

```sh
python audio_bible/tools/record_device.py unused \
  audio_bible/tools/navigation_tour.json screenshots-out/new-recording \
  design/product-videos/new-date/navigation-tour.mp4 --export-only
```

After a manual capture, restore normal firmware:

```sh
env -u BIBLE_SCREENSHOTS idf.py -C audio_bible -p PORT \
  reconfigure build app-flash
```

## Editing the tour

Each JSON item has a human-readable `label`, a `wait` in seconds (default 1.5),
and one action: `tap: [x,y]`, `swipe: [x1,y1,x2,y2]`, or `scroll: y` for an
animated move to an absolute list offset. A wait-only item holds the shot.
Optional `expect_page` stops a take if navigation reaches the wrong page.
Page IDs are the `PAGE_*` enum in `app_ui.c` (Home=0 through Favourites=10).

Tap coordinates assume the sample card contents and favourites used for the
published tour. Recheck coordinates and labels after changing layouts or card contents;
do a short take first. Keep destructive actions on Cancel. A `scene` action is
available for technical previews, but the product tour uses normal navigation.

## Capture implementation and checks

`main/capture_video.inc` is compiled only with `BIBLE_SCREENSHOTS=1`. After an
actual LCD flush, it copies the frame into one of eight bounded PSRAM buffers.
A separate writer task uses RGB565 run-length encoding and the ESP32-S3 ROM's
deflate compressor, then sends base64 over USB. The compression task needs a
32KB stack for ROM deflate.
USB never waits inside the display flush. Capture resources do not exist in
normal firmware.

Wire commands: `VIDEO START`, `TOUCH x y 0|1`, `GLIDE y`, `VIDEO STOP`.
`FRAME` headers carry sequence, device-relative milliseconds and compressed
length; `V:` lines contain base64; `ENDFRAME` closes each frame. The host validates
sequence, byte length, decompression and all 129,600 pixels. The stop result
reports captured/dropped frames. Review these counts, navigation assertions,
the full video and chapter frames before publishing a take.

Do not run recording and a flash/serial monitor at the same time. Do not touch
the screen during a scripted take. Capture mode keeps USB and the display active;
use normal firmware to evaluate charging, idle power or daily use.
