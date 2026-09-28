#!/usr/bin/env python3
"""Run non-destructive UI regression checks on a screenshot-enabled device.

Usage: python audio_bible/tools/check_ui.py PORT OUTPUT_LOG
Keep the port free of flash tools and monitors while this finite job runs.
"""
import argparse
from pathlib import Path

from capture_gallery import Device

CHECKS = (
    "library-benchmark",
    "check-library-rows",
    "check-shared-views",
    "navigation-benchmark",
    "check-bottom-targets",
    "check-back-response",
    "check-navigation-frames",
    "check-accidental-touch",
    "check-confirmations",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    failures = []
    with args.log.open("w") as log:
        device = Device(args.port, log)
        try:
            device.ready()
            for check in CHECKS:
                ok, *_ = device.ui("SCENE " + check)
                print(f'{"PASS" if ok else "FAIL"}: {check}', flush=True)
                if not ok:
                    failures.append(check)
            device.ui("SCENE home")
        finally:
            device.ser.close()
    if failures:
        raise SystemExit("UI regression failures: " + ", ".join(failures))
    print(f"All checks passed. Evidence: {args.log}")


if __name__ == "__main__":
    main()
