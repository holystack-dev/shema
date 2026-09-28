# Device UI review and product screenshots

For the navigation video, see [the reusable recording workflow](RECORDING.md),
[recorder script](record_device.py) and [editable tour plan](navigation_tour.json).

## Published set

[`design/product-screenshots/2026-09-26`](../../design/product-screenshots/2026-09-26/README.md)
holds 80 unmodified PNGs across 37 routes/states, with `index.html`, route/fixture
metadata and SHA-256 checksums. Working captures go to `screenshots-out/`, which is
git-ignored.

## One-command review

From the repository root, with the device attached and all serial monitors closed
(`PORT` is the board's serial port, e.g. `/dev/cu.usbmodem101` on macOS or
`/dev/ttyACM0` on Linux):

```sh
bash audio_bible/tools/review_device.sh PORT screenshots-out/review-YYYY-MM-DD-HHMM
```

Use a fresh output directory for each run. The script activates ESP-IDF,
flashes **only the application**, runs the non-destructive UI suite, captures
every route in `capture_catalog.py`, exports one complete set and ZIP (ZIPs are git-ignored), then
rebuilds/flashes **normal firmware**. Its exit trap also attempts that release
restore after an error or interrupt. Check `release.log` for `Hash of data verified`
and `Done`; if the device is unplugged, restore manually once it is reconnected.
Capture mode keeps the display awake and USB active, so it must not be left
installed for normal use.

Reference setup:

- Board: Waveshare ESP32-S3-Touch-LCD-1.85C V2; round 360×360 RGB565 panel.
- Port: find it with `ls /dev/cu.usbmodem*` (macOS) or `ls /dev/ttyACM*` (Linux).
- ESP-IDF: v5.3.2, `$HOME/esp/esp-idf-v5.3.2/export.sh`.
- Python: selected by the IDF export script; it includes `pyserial` and `esptool`.
- Application offset `0x20000`, 16 MB flash, DIO 80 MHz, flash baud 460800.
  `idf.py app-flash` reads the project's generated flash arguments; do not erase
  flash, reflash the partition table or format the SD card for a UI review.

For another IDF installation, set `IDF_EXPORT_SCRIPT=/path/to/export.sh` before
running the wrapper. If already activated, `IDF_PATH/export.sh` is used; otherwise
it defaults to `$HOME/esp/esp-idf-v5.3.2/export.sh`. No new Python packages or image
libraries are needed. Host progress tests additionally use the system C compiler.

## Export for the website

To export an already captured gallery without touching the device:

```sh
python3 audio_bible/tools/export_screenshots.py \
  screenshots-out/REVIEW/captures design/product-screenshots/NEW-SET
```

The destination must be empty/new. The exporter uses only manifest-listed PNGs,
checks that every route was captured, copies the gallery and metadata, records
checksums and writes a sibling ZIP. Keep 360×360 originals; apply a circle mask
in the website/device mockup. `preview-*` files are real hardware renders of
explicit sample states, not evidence of playing audio. See the exported README
for suggested Home/player/library/year/book images and fixture details.

## Manual steps and protocol

Load the ESP-IDF environment as described in the project README, then build and
flash the capture firmware:

```sh
BIBLE_SCREENSHOTS=1 idf.py -C audio_bible -p PORT reconfigure build app-flash
python audio_bible/tools/capture_gallery.py PORT screenshots-out/review
```

The tool keeps one serial connection, visits the real page builders, and writes
360×360 PNGs, an HTML gallery, a JSON manifest, and a serial log. Opening this
board's USB connection may reset it; reconnecting per screenshot loses the page.

For a short review of specific screens, use `--scenes`. Successive batches merge
into the same manifest and gallery, replacing only the requested routes:

```sh
python audio_bible/tools/capture_gallery.py PORT screenshots-out/review --scenes home versions settings
```

All Settings sections and picker options are covered. Repeated long content
lists use top, middle and bottom samples. `preview-*` routes render sample
view fixtures (empty/no-card/playback states) on the hardware. They are labelled
in the gallery and manifest; they do not erase progress, select settings, alter
favourites, or start audio. `splash` re-displays the actual embedded boot image.
The serial protocol also accepts `PING`, `SCENE <name>`, `SCROLL <y>` and
`SHOT <name>`. `capture_catalog.py` contains the shared route list. `SCENE check-confirmations` also
exercises the real list-removal and clear-history opening handlers, then Cancel
and outside-tap dismissal, checking that saved-data counts remain unchanged. It
never confirms a destructive action.

`SCENE check-bottom-targets` checks every pixel of the 80×64 Home and 128×64
Back targets, 17 Home pointer taps (including edge/drift cases), and left/right
edge Back taps on ten routes. It also checks that Back cannot intercept touches
above y290 and that modals block the controls underneath. It uses the real LVGL input state machine
with a virtual pointer; Home audio is replaced by a click counter.
This checks UI routing without starting playback and does not measure the
physical touch sensor.

Run the complete non-destructive regression suite while no flash tool or serial
monitor owns the port:

```sh
python audio_bible/tools/check_ui.py PORT screenshots-out/ui-check.log
python3 audio_bible/tools/test_played_cache.py
```

The hardware suite scrolls the shared lists and grids, verifies model IDs, text and
hit routing, traverses all 365 BIY days and 150 Psalms chapters, and checks Library
Next/Previous/letter navigation. It also measures actual page rebuilding on Back.
`LIBPERF` and `BACKPERF` timings exclude serial capture settling and navigation
animation; they are firmware work times, not physical sensor latency. The native
progress test uses a temporary host directory and never accesses the user's SD card.

`SCENE check-back-response` keeps navigation animations enabled. It checks that
Back acts on touch-down on all ten page types, a held finger never navigates twice
or activates Home Play, and Back works during the forward fade and again
immediately after release. The footer geometry test runs with animations disabled. Physical touch-controller misses still require
a live trace: capture firmware logs `TOUCH raw down/up` alongside Back events.

`SCENE check-navigation-frames` renders 60 actual framebuffer samples: forward and
Back on all ten page types, at the start, middle and end opacity of the fade.
It checks 360 background pixels inside the round glass for brightness spikes.
This catches a white display backdrop that settled screenshots cannot reveal.

`SCENE check-accidental-touch` drives LVGL pointer presses, motion and releases
through Home, Player, Home Play, Settings and confirmation controls. It checks
out-and-back drags, tolerated 4px jitter, cancellation, duplicate releases and
recycled row identity. Switch persistence and playback callbacks are replaced
with counters for those tests; user settings, favourites and history are preserved.

Tap-audio mixing has a separate native test. From `audio_bible`, run:

```sh
cc -Wall -Wextra -Werror -fsanitize=address,undefined -Icomponents/app_player \
  tools/test_ui_click.c components/app_player/ui_click.c -lm -o /tmp/test-ui-click
/tmp/test-ui-click
```

It verifies 10ms output at every MP3 sample rate, stereo, mixing across buffer
boundaries, saturation without integer wrap, and cancellation. Audio quality and
touch sensitivity are not covered and need checking on the device.

Capture support stays out of release builds. It keeps the screen
awake and the USB driver active, so return the device to normal firmware after
reviewing the captures:

```sh
env -u BIBLE_SCREENSHOTS idf.py -C audio_bible -p PORT reconfigure build app-flash
```

The capture definition affects only `main` and `app_ui`; switching modes does
not require recompiling ESP-IDF or LVGL. Flashing only the application preserves
NVS settings and the SD card contents.

## Troubleshooting

- Use **one serial owner at a time**. Wait until app-flash reports `Done` before
  opening a monitor/check/capture job. A second connection can interrupt flash
  verification or steal screenshot bytes. The review wrapper serialises these steps.
- Opening this board's USB connection can reset it. Keep the same `Device` object
  for a full job. `ready()` waits for boot and verifies a PING round trip; never
  open/close the port for each screenshot.
- Screenshots are streamed as base64 `S:` lines between named `<<<SHOT ...>>>`
  markers. `Device.shot` verifies the frame name, 360×360 dimensions and all
  259,200 RGB565 bytes before writing an RGB PNG. Raw frame data stays out of logs.
- `SCENE` and `SCROLL` wait 450ms to settle the frame. Subtracting this from host
  timings is approximate; prefer the firmware `LIBPERF` / `BACKPERF` measurements
  for profiling. They do not include the physical touch controller's latency.
- `check_ui.py` exits nonzero if any regression group fails. Logs capture every
  period, paging result and Back route. Do not interpret a gallery alone as a
  functional test. Confirmations are opened/cancelled, never confirmed on user data.
- The script preserves NVS and card contents; user activity during a review can
  change counts.
- `virtual_list.inc` and `virtual_grid.inc` own reusable rendering; their contracts
  are in `components/app_ui/REUSABLE_VIEWS.md`. Add new UI routes to the capture
  route table and `capture_catalog.py` together.
