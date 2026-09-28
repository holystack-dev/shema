# Firmware asset pack

- `icons/*.svg`: exported icon vectors. Edit the source paths in `generate_icons.py`.
- `icons.json`, `icons_preview.png`: icon list and a native-size preview sheet.
- `manifest.json`: asset dimensions and checksums.

The firmware uses these generated and converted assets:

- `audio_bible/components/app_ui/shema_icons.c` / `.h`: 35 icons at three sizes (24/28/32) as static LVGL 8 alpha masks. No SVG decoder or font file is required.
- `audio_bible/components/app_ui/shema_theme.h`: palette, geometry and font mapping.
- `audio_bible/main/assets/splash.png` / `splash.bin`: 360 × 360 splash; the `.bin` is RGB565 high byte first, matching `CONFIG_LV_COLOR_16_SWAP=y`.

Usage: `shema_icon_create(button, SHEMA_ICON_BOOK, 28, color)` returns a centred-ready image object. The icon is decorative; attach events to the parent button. `shema_icon_set_color` recolours it on theme change, and `shema_icon_set_symbol` switches between outline and filled variants (heart, moon) without recreating the widget.

Cards, buttons, tracks, switches, progress and modal surfaces are drawn with native LVGL styles. The UI uses LVGL's built-in Montserrat fonts.

Regenerate icons (writes `shema_icons.c`/`.h` into `audio_bible/components/app_ui/` and refreshes `icons/`, `icons.json`, the preview sheet and `manifest.json`):

    python3 design/shema-v2/firmware-assets/generate_icons.py

Regenerate the RGB565 splash:

    python3 audio_bible/tools/png_to_rgb565.py audio_bible/main/assets/splash.png audio_bible/main/assets/splash.bin
