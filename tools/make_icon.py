"""Regenerates assets/icon.ico from the vector drawings.

Usage (repo root, any shell; Python 3 standard library only):

    python tools/make_icon.py                 writes assets/icon.ico
    python tools/make_icon.py --sheet out.png also writes a contact sheet of every frame
                                              on a dark and a light background

Each frame is rendered from the SVG at its own pixel size (never scaled from another frame):
assets/icon-small.svg for 16 to 24 px, assets/icon.svg for 32 px and up. Frames 16 to 128 are
stored as 32-bit BGRA DIBs with an AND mask, the 256 frame as PNG, the layout rc.exe and the
Windows shell accept.

The rasterizer is tools/icon/svg2rgba.exe built from lunasvg (the renderer Facet uses for SVG,
so the title bar and the .ico agree). Build it once, inside the MSVC developer environment:

    cmake -S tools/icon -B build/icon-tool -G Ninja -DCMAKE_BUILD_TYPE=Release
          -DLUNASVG_SOURCE_DIR=build/release/_deps/facet_lunasvg-src
    cmake --build build/icon-tool

Pass --renderer <path> if the exe lives elsewhere.

app.rc embeds icon.ico and both SVGs (the title bar draws the SVGs), but the build does not see
changes to them: touch app.rc after editing the SVGs so the resource recompiles.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FRAME_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SMALL_MAX = 24  # frames up to this size use the simplified drawing


# --- Rendering -----------------------------------------------------------------------------------

def render(renderer: str, svg: str, sizes) -> dict[int, bytes]:
    """Returns {size: straight-alpha RGBA bytes, top-down}."""
    sizes = list(sizes)
    with tempfile.TemporaryDirectory() as tmp:
        prefix = os.path.join(tmp, "f")
        subprocess.run([renderer, svg, prefix] + [str(s) for s in sizes], check=True)
        out = {}
        for s in sizes:
            with open(f"{prefix}{s}.rgba", "rb") as f:
                data = f.read()
            if len(data) != s * s * 4:
                raise RuntimeError(f"unexpected raster size for {s}px")
            out[s] = data
    return out


# --- PNG -----------------------------------------------------------------------------------------

def png_encode(width: int, height: int, rgba: bytes) -> bytes:
    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    stride = width * 4
    raw = b"".join(b"\x00" + rgba[y * stride:(y + 1) * stride] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


# --- ICO -----------------------------------------------------------------------------------------

def dib_frame(size: int, rgba: bytes) -> bytes:
    """BITMAPINFOHEADER + bottom-up BGRA + 1-bit AND mask (set where fully transparent)."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    stride = size * 4
    pixels = bytearray()
    for y in range(size - 1, -1, -1):
        row = rgba[y * stride:(y + 1) * stride]
        for x in range(size):
            r, g, b, a = row[x * 4:x * 4 + 4]
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(mask_stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + bytes(pixels) + bytes(mask)


def write_ico(path: str, frames: dict[int, bytes]) -> None:
    sizes = sorted(frames)
    blobs = [png_encode(s, s, frames[s]) if s >= 256 else dib_frame(s, frames[s]) for s in sizes]
    out = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for s, blob in zip(sizes, blobs):
        dim = 0 if s >= 256 else s
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for blob in blobs:
        out += blob
    with open(path, "wb") as f:
        f.write(out)


# --- Contact sheets ------------------------------------------------------------------------------

class Canvas:
    def __init__(self, width: int, height: int, color=(0, 0, 0)):
        self.w, self.h = width, height
        self.px = bytearray(bytes((*color, 255)) * (width * height))

    def fill(self, x0: int, y0: int, x1: int, y1: int, color) -> None:
        row = bytes((*color, 255)) * (x1 - x0)
        for y in range(y0, y1):
            i = (y * self.w + x0) * 4
            self.px[i:i + len(row)] = row

    def blit(self, x0: int, y0: int, size: int, rgba: bytes, scale: int = 1) -> None:
        """Alpha-composites an RGBA image, optionally magnified with nearest-neighbour pixels."""
        for sy in range(size):
            for sx in range(size):
                j = (sy * size + sx) * 4
                a = rgba[j + 3]
                if a == 0:
                    continue
                for dy in range(scale):
                    y = y0 + sy * scale + dy
                    i = (y * self.w + x0 + sx * scale) * 4
                    for _ in range(scale):
                        for c in range(3):
                            self.px[i + c] = (rgba[j + c] * a + self.px[i + c] * (255 - a) + 127) // 255
                        i += 4

    def save(self, path: str) -> None:
        with open(path, "wb") as f:
            f.write(png_encode(self.w, self.h, bytes(self.px)))


def frame_set(renderer: str, master: str, small: str | None) -> dict[int, bytes]:
    small_sizes = [s for s in FRAME_SIZES if s <= SMALL_MAX] if small else []
    big_sizes = [s for s in FRAME_SIZES if s not in small_sizes]
    frames = render(renderer, master, big_sizes)
    if small_sizes:
        frames.update(render(renderer, small, small_sizes))
    return frames


def sheet(path: str, frames: dict[int, bytes]) -> None:
    """Two rows (dark, light): every frame at 1x, then 16/24/32 magnified 8x/6x/4x."""
    backgrounds = ((0x1C, 0x1C, 0x1E), (0xF3, 0xF3, 0xF3))
    pad = 24
    sizes = sorted(frames)
    zooms = [(16, 8), (24, 6), (32, 4)]
    row_h = 256 + 2 * pad
    width = pad + sum(s + pad for s in sizes) + sum(s * z + pad for s, z in zooms)
    canvas = Canvas(width, row_h * 2)
    for r, bg in enumerate(backgrounds):
        canvas.fill(0, r * row_h, width, (r + 1) * row_h, bg)
        x = pad
        for s in sizes:
            canvas.blit(x, r * row_h + pad + 256 - s, s, frames[s])
            x += s + pad
        for s, z in zooms:
            canvas.blit(x, r * row_h + pad + 256 - s * z, s, frames[s], z)
            x += s * z + pad
    canvas.save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--renderer", default=os.path.join(ROOT, "build", "icon-tool", "svg2rgba.exe"))
    parser.add_argument("--master", default=os.path.join(ROOT, "assets", "icon.svg"))
    parser.add_argument("--small", default=os.path.join(ROOT, "assets", "icon-small.svg"))
    parser.add_argument("--out", default=os.path.join(ROOT, "assets", "icon.ico"))
    parser.add_argument("--sheet", help="also write a contact sheet PNG here")
    args = parser.parse_args()
    if not os.path.isfile(args.renderer):
        print(f"renderer not found: {args.renderer}\nbuild it first, see the comment at the top of this file",
              file=sys.stderr)
        return 1
    small = args.small if os.path.isfile(args.small) else None
    frames = frame_set(args.renderer, args.master, small)
    write_ico(args.out, frames)
    print(f"wrote {args.out}: {', '.join(str(s) for s in sorted(frames))} px")
    if args.sheet:
        sheet(args.sheet, frames)
        print(f"wrote {args.sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
