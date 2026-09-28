# Shema UI concept

These are concept mockups for the firmware UI. The shipped firmware keeps the original information density and navigation; see `audio_bible/components/app_ui/`. [firmware-assets](firmware-assets/README.md) holds the icon sources and generator.

Open `index.html` for the clickable navigation mockup. `overview.png` and `overview.svg` show sixteen key screens. The 39 individual SVGs use exact 360 × 360 coordinates. For device captures of the firmware, see [`../product-screenshots/`](../product-screenshots/2026-09-26/README.md).

## Page mapping

Firmware pages are defined in `../../audio_bible/components/app_ui/app_ui.c`.

| Firmware page | Concept files |
| --- | --- |
| HOME | home, more, no-card |
| BOOKS | books, book-list, book-list-2 |
| CHAPTERS | chapters, chapters-2 |
| PLAYER | player, player-paused, player-options, volume, sleep |
| LIST_DETAIL | saved, saved-edit |
| SETTINGS | settings through settings-4, versions |
| BIY | year |
| BIY_DAYS | days, intros |
| RECENT | recent |
| LIB | library, folder, letters |
| FAVOURITES | favourites |

Sample books, tracks, language choices, durations and progress are illustrative. Each firmware page type is represented; not every data-dependent state or modal is reproduced.

## Design measurements

- Display: 360 × 360 circle, using the repository's stated 1.85 inch diameter.
- Minimum button dimensions: 64 × 64 px, approximately 8.4 mm on the stated display.
- Main play/pause: 80 px, approximately 10.4 mm.
- Home tiles: 100 × 72 px; chapter buttons: 64 × 64 px with 12 px gutters.
- Lists: two 240 × 64 px rows, 12 px apart. Additional content uses paging or scrolling.
- Body text: 18 px; home labels: 16 px; secondary metadata: 14 px; headings: 21–31 px.
- Midnight #080D13, card #17212B, champagne #E8BE78, ivory #F7F1E6, secondary #AFBDC7.
- Original line icons use a consistent 24 px drawing grid and 1.8 px stroke. They are vectors, not font glyphs or emoji.
- The player separates its three large transport buttons from volume, sleep and saving. Those secondary actions remain available through More.
- Home shows Bible, In a Year, Library and More. More provides Recent, Favourites and Settings. A Resume button remains directly accessible.
- The firmware keeps capability-based visibility, long-title truncation, tap-versus-scroll protection, listening history, battery/charging status, and contextual back navigation.

## Prototype scope

The local HTML demonstrates contextual Back navigation, chapter selection, play/pause, previous/next chapter, bounded volume and brightness adjustment, sleep choices, settings toggles, theme switching, saving and clearing sample history. It starts with the splash; tap it to enter Home. Audio and SD-card data are illustrative; some library and yearly-plan items share sample destinations. Static SVGs show default states; open index.html for interactions. All button regions meet the 64 px minimum and fit inside the circular screen.

## Splash

`splash-bible-master.png` is the high-resolution open-Bible artwork, with JOHN / CHAPTER 1 headings and printed Scripture detail. Its 360 × 360 export is `audio_bible/main/assets/splash.png`, converted to RGB565 as `splash.bin`. At 360 px the small print serves as page texture.

Rebuild SVGs and HTML with `python3 design/shema-v2/build_mockup.py` from the repository root.
