# Shema navigation video

[Watch with chapter navigation](index.html) · [Download MP4](navigation-tour.mp4)

- 2:38 walkthrough across all 11 page types and their principal dialogs.
- Real 360×360 display frames from the connected ESP32; silent screen recording.
- 336 source display updates, zero dropped frames. The 30fps H.264 export repeats
  existing frames between updates; no generated motion or speed-up.
- Website MP4: 1,165,170 bytes, yuv420p, fast-start metadata. Poster: `navigation-tour.png`.
- `navigation-tour.json` lists every tour action and its timing.

The sequence covers Bible books/chapters, yearly periods/days/introductions,
nested Library folders, Songs and alphabet navigation, all favourite collections,
Edit and Cancel, a saved passage, Play/Pause, filled favourite and sleep icons,
Recent, version choices, timeout/power-off options, theme colours and clear-history
confirmation followed by Cancel. The tour does not confirm deletions or change
persistent Settings.

## Reuse

- [Recorder script](../../../audio_bible/tools/record_device.py)
- [Editable shot plan](../../../audio_bible/tools/navigation_tour.json)
- [Build, record, export and firmware-restore guide](../../../audio_bible/tools/RECORDING.md)

For a website, retain the original resolution and apply a circular CSS mask.
The included preview has accessible controls outside the round screen and chapter
buttons. Use the MP4/PNG directly in the website's own player if preferred.
