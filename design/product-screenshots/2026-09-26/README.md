# Shema product UI screenshots

One complete set: **80 original PNGs covering 37 routes/states**.
Open `index.html` to browse. `manifest.json` maps each file to its route, scroll
position and whether it is a sample preview fixture. No source images were
resized, retouched, enhanced or composited.

## Website use

- Native resolution: 360×360 RGB PNG, captured from the connected ESP32 framebuffer.
- Use the PNGs as the screen inside device photography/mockups. Apply a circular
  mask (`border-radius: 50%`) for the round panel; keep the originals unchanged.
- `home.png`, `preview-player-playing.png`, `preview-player-year.png`,
  `preview-player-library.png`, `year-01.png`, `library-01.png` and
  `books-old-01.png` are useful starting points for product sections.
- `preview-*` images use labelled sample view state on real hardware. They show
  playing/paused, long-title, empty and no-card variants without changing user data.
  Other routes use the card contents and saved settings at capture time.
- Numbered filenames are top/middle/bottom positions or consecutive sections of
  longer settings/dialogs. They are distinct views of the same route, not versions.
- This is UI artwork, not a photograph of the physical enclosure. Prefer native
  size or smaller; avoid presenting an upscale as a higher-resolution display.

`SHA256SUMS.json` lists checksums of the exported files. The capture workflow is
documented in `audio_bible/tools/SCREENSHOTS.md`; run `review_device.sh` for a
firmware review and `export_screenshots.py` to create a new set.
