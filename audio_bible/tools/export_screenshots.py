#!/usr/bin/env python3
"""Export exactly one complete captured set, with a gallery and ZIP for website work."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from capture_catalog import SCENES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source, destination = args.capture_dir.resolve(), args.destination.resolve()
    records = json.loads((source / "manifest.json").read_text())
    captures = [r for r in records if r.get("status") == "captured" and "file" in r]
    if not captures or len(captures) != len(records):
        raise SystemExit("Capture set is empty or has unavailable routes; review its manifest first.")
    missing = set(SCENES) - {r["scene"] for r in captures}
    if missing:
        raise SystemExit("Incomplete route coverage: " + ", ".join(sorted(missing)))
    if len({r["file"] for r in captures}) != len(captures):
        raise SystemExit("Duplicate screenshot filenames in manifest.")
    if destination.exists() and any(destination.iterdir()):
        raise SystemExit("Choose a new or empty destination to avoid mixing capture sessions.")
    # Validate every source before creating the deliverable. Only manifest-listed
    # files are exported; stale images from previous runs cannot slip into the set.
    for record in captures:
        name = record["file"]
        if Path(name).name != name or not (source / name).is_file():
            raise SystemExit(f"Invalid or missing screenshot: {name}")
    destination.mkdir(parents=True, exist_ok=True)
    for record in captures:
        shutil.copy2(source / record["file"], destination / record["file"])
    shutil.copy2(source / "index.html", destination / "index.html")
    (destination / "manifest.json").write_text(json.dumps(captures, indent=2) + "\n")
    routes = len({r["scene"] for r in captures})
    (destination / "README.md").write_text(f"""# Shema product UI screenshots

One complete set: **{len(captures)} original PNGs covering {routes} routes/states**.
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
""")
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(destination.iterdir()) if p.is_file()}
    (destination / "SHA256SUMS.json").write_text(json.dumps(hashes, indent=2) + "\n")
    archive = destination.with_suffix(".zip")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
        for path in sorted(destination.iterdir()):
            output.write(path, arcname=destination.name + "/" + path.name)
    print(f"Exported {len(captures)} PNGs / {routes} routes to {destination}")
    print(f"Archive: {archive}")


if __name__ == "__main__":
    main()
