#!/bin/bash
# Finite review: capture firmware -> regression checks -> gallery -> normal firmware.
set -eo pipefail
if [ "$#" -ne 2 ] && { [ "$#" -ne 4 ] || [ "${3:-}" != "--video" ]; }; then
    printf 'Usage: bash %s PORT OUTPUT_DIRECTORY [--video PLAN.json]\n' "$0" >&2
    exit 2
fi
review_port=$1
review_tools=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
review_project=$(cd -- "$review_tools/.." && pwd)
mkdir -p -- "$2"
review_output=$(cd -- "$2" && pwd)
review_export=${IDF_EXPORT_SCRIPT:-${IDF_PATH:-$HOME/esp/esp-idf-v5.3.2}/export.sh}
if [ ! -f "$review_export" ]; then
    printf 'Set IDF_EXPORT_SCRIPT to the ESP-IDF v5.3.2 export.sh path.\n' >&2
    exit 2
fi
# IDF chooses its matching Python environment, including pyserial and esptool.
source "$review_export" > "$review_output/environment.log" 2>&1
set -u
review_restore_needed=0
review_cleanup() {
    review_status=$?
    trap - EXIT INT TERM
    if [ "$review_restore_needed" -eq 1 ]; then
        printf 'Restoring normal firmware; see %s/release.log\n' "$review_output"
        if ! env -u BIBLE_SCREENSHOTS idf.py -C "$review_project" -p "$review_port" \
            reconfigure build app-flash > "$review_output/release.log" 2>&1; then
            printf 'Release restore failed. Keep the logs and rerun the normal flash command in SCREENSHOTS.md.\n' >&2
            exit 1
        fi
    fi
    exit "$review_status"
}
trap review_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf 'Building capture firmware; see %s/capture-build.log\n' "$review_output"
# Mark before flashing so an interrupted flash also attempts recovery on exit.
review_restore_needed=1
BIBLE_SCREENSHOTS=1 idf.py -C "$review_project" -p "$review_port" \
    reconfigure build app-flash > "$review_output/capture-build.log" 2>&1
python "$review_tools/check_ui.py" "$review_port" "$review_output/ui-check.log"
if [ "${3:-}" = "--video" ]; then
    python "$review_tools/record_device.py" "$review_port" "$4" "$review_output/recording" \
        "$review_output/product-video/navigation-tour.mp4"
    printf 'Video and poster: %s/product-video/\n' "$review_output"
else
    python "$review_tools/capture_gallery.py" "$review_port" "$review_output/captures"
    python "$review_tools/export_screenshots.py" "$review_output/captures" "$review_output/product-screenshots"
    printf 'Complete gallery: %s/product-screenshots/index.html\n' "$review_output"
fi
