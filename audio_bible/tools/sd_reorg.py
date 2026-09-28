#!/usr/bin/env python3
"""Reorganize the audio SD card to the layout the firmware expects.

Two jobs, both idempotent and reversible (every move is logged to a manifest CSV):

1. BIBLE versions -> 2-level  BIBLE/<LANG>/<VERSION>/<book>_<chapter>.mp3
   The version scanner walks exactly two levels under BIBLE/, so the Malayalam
   editions (currently BIBLE/ML/Malayalam/POC_1) are lifted up one level:
       BIBLE/ML/Malayalam/POC_1 -> BIBLE/ML/POC_1
       BIBLE/ML/Malayalam/POC_2 -> BIBLE/ML/POC_2
   EN/NRSV already conforms.

2. BIY episodes -> readable English names with the podcast boilerplate stripped:
       Day 001 - In the Beginning.mp3      (365 day episodes)
       Intro 04 - The Early World.mp3      (15 intro/checkpoint episodes)
   Malayalam files get the English title of the same day (the device font can't
   render Malayalam glyphs). Intro titles come from the ordered lists below.

Usage:
    python3 tools/sd_reorg.py --dry-run          # preview, change nothing
    python3 tools/sd_reorg.py                     # do it (default root /Volumes/Untitled)
    python3 tools/sd_reorg.py --root /Volumes/Untitled --manifest tools/sd_rename_manifest.csv
"""
import argparse
import csv
import os
import re
import sys

# Clean English intro/checkpoint titles, in the order the episodes appear on disk.
EN_INTROS = [
    "What Catholics Are Saying", "Praise for the BIY", "Preparing for the Journey",
    "The Early World", "The Patriarchs", "Egypt and Exodus", "Conquest and Judges",
    "Gospel of John", "Gospel of Mark", "Divided Kingdom", "The Exile", "The Return",
    "Maccabean Revolt", "Gospel of Luke", "The Church",
]
ML_INTROS = [
    "The Early World", "The Patriarchs", "Egypt and Exodus", "Desert Wanderings",
    "Conquest and Judges", "Checkpoint 1", "Royal Kingdom", "Checkpoint 2",
    "Divided Kingdom", "The Exile", "Checkpoint 3", "The Return", "Maccabean Revolt",
    "Messianic Fulfillment", "The Church",
]

FAT_ILLEGAL = re.compile(r'[\\/:*?"<>|]')
# Map common smart punctuation to ASCII; anything else non-ASCII is dropped, so
# titles stay in the device font + filesystem's ASCII range (e.g. "Rehobo'am").
SMART = {
    "‘": "'", "’": "'", "′": "'", "“": '"', "”": '"',
    "″": '"', "–": "-", "—": "-", "…": "...",
}


def sanitize(name):
    """Make a title safe for FAT long filenames and the device's ASCII font."""
    for k, v in SMART.items():
        name = name.replace(k, v)
    name = name.encode("ascii", "ignore").decode("ascii")   # drop any remaining non-ASCII
    name = FAT_ILLEGAL.sub("", name)
    name = re.sub(r"\s+", " ", name).strip(" .-")
    return name


def is_audio(name):
    return name.lower().endswith(".mp3") and not name.startswith("._")


def seq_of(name):
    m = re.match(r"^(\d+)", name)
    return int(m.group(1)) if m else 99999


def day_of(name, malayalam):
    pat = r"ദിവസം[：:\s]*\s*(\d+)" if malayalam else r"\bDay\s*(\d+)"
    m = re.search(pat, name)
    return int(m.group(1)) if m else None


def strip_title(name):
    """Pull a clean English title out of an original BIY day filename."""
    s = re.sub(r"\.mp3$", "", name, flags=re.I)
    s = re.sub(r"^\d+\s*[-–]\s*", "", s)                       # leading "NNN - "
    s = re.split(r"\s*[—–-]\s*The Bible in a Year", s)[0]
    s = re.split(r"The Bible in a Year", s)[0]
    s = re.split(r"\s*[｜|]\s*Fr\.", s)[0]
    s = re.sub(r"\(with[^)]*\)", "", s)
    s = re.sub(r"\(Fr\.[^)]*\)", "", s)
    s = re.sub(r"^Day\s*\d+\s*[：:]\s*", "", s)                 # "Day N:" prefix
    return s.strip(" -–—｜|'\"")


TARGET_DAY = re.compile(r"^Day (\d{3}) - (.+)\.mp3$")
TARGET_INTRO = re.compile(r"^Intro (\d{2}) - (.+)\.mp3$")


def build_en_day_titles(en_dir):
    """day -> English title, reading whichever form the EN folder is currently in."""
    titles = {}
    if not os.path.isdir(en_dir):
        return titles
    for n in os.listdir(en_dir):
        if not is_audio(n):
            continue
        m = TARGET_DAY.match(n)                                # already renamed
        if m:
            titles[int(m.group(1))] = m.group(2)
            continue
        d = day_of(n, malayalam=False)                         # original form
        if d:
            titles[d] = sanitize(strip_title(n))
    return titles


def plan_folder(folder, en_day_titles, intro_titles, malayalam):
    """Return list of (old_name, new_name) for one BIY language folder."""
    if not os.path.isdir(folder):
        return []
    files = sorted((n for n in os.listdir(folder) if is_audio(n)), key=seq_of)
    rows, intro_i = [], 0
    for n in files:
        if TARGET_DAY.match(n) or TARGET_INTRO.match(n):       # already done
            continue
        d = day_of(n, malayalam)
        if d is not None:
            title = en_day_titles.get(d) or sanitize(strip_title(n))
            new = f"Day {d:03d} - {title}.mp3"
        else:
            title = intro_titles[intro_i] if intro_i < len(intro_titles) else f"Intro {intro_i+1}"
            new = f"Intro {intro_i+1:02d} - {sanitize(title)}.mp3"
            intro_i += 1
        if new != n:
            rows.append((n, new))
    return rows


def do_rename(folder, rows, dry, manifest):
    for old, new in rows:
        src, dst = os.path.join(folder, old), os.path.join(folder, new)
        if os.path.exists(dst):
            print(f"  SKIP (target exists): {new}", file=sys.stderr)
            continue
        print(f"  {old[:55]:55s} -> {new}")
        if not dry:
            os.rename(src, dst)
            manifest.append((src, dst))


def do_move(src, dst, dry, manifest):
    if not os.path.isdir(src) or os.path.exists(dst):
        return
    print(f"  MOVE {src} -> {dst}")
    if not dry:
        os.rename(src, dst)
        manifest.append((src, dst))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="/Volumes/Untitled")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--manifest",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "sd_rename_manifest.csv"))
    args = ap.parse_args()
    root, dry = args.root, args.dry_run
    if not os.path.isdir(root):
        sys.exit(f"SD root not found: {root}")
    manifest = []

    print(f"== BIBLE: normalize to 2-level under {root}/BIBLE ==")
    ml = os.path.join(root, "BIBLE", "ML")
    mal = os.path.join(ml, "Malayalam")
    for ver in ("POC_1", "POC_2"):
        do_move(os.path.join(mal, ver), os.path.join(ml, ver), dry, manifest)
    if os.path.isdir(mal) and not os.listdir(mal) and not dry:
        os.rmdir(mal)
        print(f"  rmdir {mal}")

    biy = os.path.join(root, "BIY")
    en_titles = build_en_day_titles(os.path.join(biy, "EN"))
    if en_titles:
        missing = [d for d in range(1, 366) if d not in en_titles]
        if missing:
            print(f"  WARNING: EN missing day titles: {missing[:10]}", file=sys.stderr)
    print(f"\n== BIY/EN ==")
    do_rename(os.path.join(biy, "EN"),
              plan_folder(os.path.join(biy, "EN"), en_titles, EN_INTROS, False), dry, manifest)
    print(f"\n== BIY/ML (English titles) ==")
    do_rename(os.path.join(biy, "ML"),
              plan_folder(os.path.join(biy, "ML"), en_titles, ML_INTROS, True), dry, manifest)

    if manifest and not dry:
        new = not os.path.exists(args.manifest)
        with open(args.manifest, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["old", "new"])
            w.writerows(manifest)
        print(f"\nManifest: {len(manifest)} ops appended to {args.manifest}")
    elif dry:
        print("\n(dry run — nothing changed)")


if __name__ == "__main__":
    main()
