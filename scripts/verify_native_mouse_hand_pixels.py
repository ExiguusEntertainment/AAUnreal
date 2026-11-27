"""Compare the rendered mouse-hand bitmap with original CPC and GAME.PAL pixels.

Requires Pillow. Transparent CPC pixels are excluded so the world can remain visible
through the ring. No source assets or screenshot files are modified.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct

from PIL import Image


PROJECT = Path(__file__).resolve().parents[1]
ART = PROJECT / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal"


def original_pixels(sprite: Path, palette: Path):
    data = sprite.read_bytes()
    raw_palette = palette.read_bytes()
    if len(data) < 4 or len(raw_palette) < 256 * 3:
        raise ValueError("Incomplete CPC header or original palette")
    height, width = struct.unpack_from("<HH", data)
    columns_end = 4 + width * 4
    if not (1 <= width <= 65535 and 1 <= height <= 256) or columns_end > len(data):
        raise ValueError("Invalid CPC dimensions/column table")
    colors = [tuple((value & 63) * 255 // 63 for value in raw_palette[n:n + 3])
              for n in range(0, 256 * 3, 3)]
    opaque = []
    for x in range(width):
        offset, top, bottom = struct.unpack_from("<HBB", data, 4 + x * 4)
        if top == bottom == 255:
            continue
        if top > bottom or bottom >= height or offset < columns_end or offset + bottom - top >= len(data):
            raise ValueError(f"Invalid CPC column {x}")
        for y in range(top, bottom + 1):
            index = data[offset + y - top]
            if index:  # LoadWadSprite leaves palette index zero transparent.
                opaque.append((x, y, colors[index]))
    if not opaque:
        raise ValueError("CPC has no opaque pixels")
    return width, height, opaque


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("screenshot", nargs="?", type=Path,
                        default=PROJECT / "Saved/PlayableLoops-RetainedHand-Rendered-held-inventory.png")
    parser.add_argument("--center", nargs=2, type=float, default=(360, 210), metavar=("X", "Y"))
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--sprite", type=Path, default=ART / "OBJS/RING17/RING17.CPC")
    parser.add_argument("--palette", type=Path, default=ART / "GAME.PAL")
    args = parser.parse_args()
    try:
        if args.scale < 1:
            raise ValueError("Scale must be a positive integer")
        width, height, opaque = original_pixels(args.sprite, args.palette)
        left = args.center[0] - width * args.scale / 2
        top = args.center[1] - height * args.scale / 2
        if abs(left - round(left)) > 1e-6 or abs(top - round(top)) > 1e-6:
            raise ValueError("Choose a center/scale with a pixel-aligned bitmap origin")
        left, top = round(left), round(top)
        with Image.open(args.screenshot) as opened:
            screenshot = opened.convert("RGB")
        if left < 0 or top < 0 or left + width * args.scale > screenshot.width or top + height * args.scale > screenshot.height:
            raise ValueError("Expected bitmap extends beyond the screenshot")
        matched = total = 0
        first_mismatch = None
        for x, y, expected in opaque:
            for dy in range(args.scale):
                for dx in range(args.scale):
                    point = (left + x * args.scale + dx, top + y * args.scale + dy)
                    actual = screenshot.getpixel(point)
                    total += 1
                    matched += actual == expected
                    if actual != expected and first_mismatch is None:
                        first_mismatch = (point, expected, actual)
        print(f"{'PASS' if matched == total else 'FAIL'}: {matched}/{total} opaque pixels match "
              f"the original {width}x{height} CPC at center {tuple(args.center)}, scale {args.scale}.")
        if first_mismatch:
            point, expected, actual = first_mismatch
            print(f"First mismatch at {point}: expected {expected}, found {actual}.")
        return 0 if matched == total else 1
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
