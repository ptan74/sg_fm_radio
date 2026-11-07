#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render the FM dial scale strip used by components/app_radio_ui/ui/main.json.

The strip covers 87.5-108.0 MHz at PX_PER_MHZ pixels per MHz with half a
screen of padding on each side, so the UI can slide it under a fixed centre
needle. Station markers come from the `.freq_tenths = N` entries in
components/app_stations/app_stations.c. Geometry constants must match
components/app_radio_ui/app_radio_ui.c (DIAL_*).

Requires Pillow:  python -m pip install pillow
Usage:            python tools/gen_dial_strip.py
"""
from __future__ import annotations

import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
STATIONS_C = ROOT / "components/app_stations/app_stations.c"
FONT = ROOT / "components/app_radio_ui/ui/fonts/DejaVuSans.ttf"
OUT = ROOT / "components/app_radio_ui/ui/dial_strip.png"

BAND_MIN_TENTHS = 875
BAND_MAX_TENTHS = 1080
PX_PER_MHZ = 60
PAD = 240  # half the 480 px screen
HEIGHT = 84
BASELINE = 46

BG = (0x14, 0x13, 0x0F)
MAJOR = (0xD3, 0xD1, 0xC7)
MINOR = (0x5F, 0x5E, 0x5A)
LABEL = (0xB4, 0xB2, 0xA9)
MARKER = (0xEF, 0x9F, 0x27)


def station_tenths() -> list[int]:
    text = STATIONS_C.read_text(encoding="utf-8")
    values = [int(v) for v in re.findall(r"\.freq_tenths\s*=\s*(\d+)", text)]
    if not values:
        raise SystemExit(f"no stations found in {STATIONS_C}")
    return values


def x_for(tenths: int) -> int:
    return PAD + (tenths - BAND_MIN_TENTHS) * PX_PER_MHZ // 10


def main() -> None:
    width = 2 * PAD + (BAND_MAX_TENTHS - BAND_MIN_TENTHS) * PX_PER_MHZ // 10
    img = Image.new("RGB", (width, HEIGHT), BG)
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(str(FONT), 14)

    for tenths in range(BAND_MIN_TENTHS, BAND_MAX_TENTHS + 1):
        x = x_for(tenths)
        if tenths % 10 == 0:
            draw.rectangle([x, BASELINE - 32, x + 1, BASELINE], fill=MAJOR)
            label = str(tenths // 10)
            w = draw.textlength(label, font=font)
            draw.text((x + 1 - w / 2, BASELINE + 6), label, font=font, fill=LABEL)
        elif tenths % 5 == 0:
            draw.rectangle([x, BASELINE - 20, x, BASELINE], fill=MAJOR)
        else:
            draw.rectangle([x, BASELINE - 10, x, BASELINE], fill=MINOR)

    for tenths in station_tenths():
        x = x_for(tenths)
        draw.ellipse([x - 4, HEIGHT - 12, x + 4, HEIGHT - 4], fill=MARKER)

    img.save(OUT, optimize=True)
    print(f"wrote {OUT.relative_to(ROOT)} ({width}x{HEIGHT})")


if __name__ == "__main__":
    main()
