#!/usr/bin/env python3
"""Rasterize Departure Mono into C bitmap headers for the ESP32 sketches.

Departure Mono is pixel-perfect at multiples of 11px. Each glyph is stored as a
7-column cell; the generated header carries the measured advance and the blank
rows above the cap/digit ink. Requires Pillow for the outline sizes; downloads
the OTF if not cached.

Below 11px the outlines land off the pixel grid, so thresholding knocks out
thin strokes (a broken X/W/A at 9px). The small label font is therefore a
hand-drawn 5x6 pixel set in the same 1px-stroke style (--pixel), which needs no
Pillow and stays crisp at any zoom.

Usage:
    python3 tools/font/make_departure_font.py --size 11 --prefix dm \
        --out pomodoro/DepartureMono.h
    python3 tools/font/make_departure_font.py --pixel --prefix dms \
        --out pomodoro/DepartureMonoSmall.h

Licensed under the SIL Open Font License 1.1 (see tools/font/OFL.txt).
"""

import argparse
import os
import urllib.request

FONT_URL = (
    "https://raw.githubusercontent.com/rektdeckard/departure-mono/"
    "main/public/assets/DepartureMono-1.500.otf"
)
CACHE = "/tmp/departure-mono/DepartureMono.otf"
CELL_W, CELL_H = 7, 14
FIRST, LAST = 32, 90  # space .. 'Z'

# Hand-drawn 5x6 pixel set (space .. 'Z'), row 0 = cap/digit top, row 5 the
# baseline, row 6 only for punctuation descenders. Emitted at cell rows 3..9.
PIXEL_ROWS, PIXEL_TOP = 6, 3
PIXEL = {
    " ": ["     "] * 6,
    "!": ["  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "],
    '"': [" # # ", " # # ", "     ", "     ", "     ", "     "],
    "#": [" # # ", " # # ", "#####", " # # ", "#####", " # # "],
    "$": ["  #  ", " ####", "# #  ", " ### ", "  # #", "#### "],
    "%": ["##  #", "## # ", "   # ", "  #  ", " # ##", "#  ##"],
    "&": [" ##  ", "#  # ", " ##  ", " #.# ", "# #.#", " ##.#"],
    "'": ["  #  ", "  #  ", "     ", "     ", "     ", "     "],
    "(": ["   # ", "  #  ", "  #  ", "  #  ", "  #  ", "   # "],
    ")": [" #   ", "  #  ", "  #  ", "  #  ", "  #  ", " #   "],
    "*": ["  #  ", "# # #", " ### ", "# # #", "  #  ", "     "],
    "+": ["     ", "  #  ", "  #  ", "#####", "  #  ", "  #  "],
    ",": ["     ", "     ", "     ", "     ", "     ", "  #  ", " #   "],
    "-": ["     ", "     ", "     ", " ### ", "     ", "     "],
    ".": ["     ", "     ", "     ", "     ", "  #  ", "  #  "],
    "/": ["    #", "   # ", "  #  ", " #   ", "#    ", "     "],
    "0": [" ### ", "#   #", "#  ##", "# # #", "##  #", " ### "],
    "1": ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "#####"],
    "2": [" ### ", "#   #", "   # ", "  #  ", " #   ", "#####"],
    "3": [" ### ", "#   #", "  ## ", "    #", "#   #", " ### "],
    "4": ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # "],
    "5": ["#####", "#    ", "#### ", "    #", "#   #", " ### "],
    "6": [" ### ", "#   #", "#    ", "#### ", "#   #", " ### "],
    "7": ["#####", "    #", "   # ", "  #  ", "  #  ", "  #  "],
    "8": [" ### ", "#   #", "#   #", " ### ", "#   #", " ### "],
    "9": [" ### ", "#   #", "#   #", " ####", "   # ", " ##  "],
    ":": ["     ", "  #  ", "  #  ", "     ", "  #  ", "  #  "],
    ";": ["     ", "  #  ", "  #  ", "     ", "  #  ", "  #  ", " #   "],
    "<": ["   # ", "  #  ", " #   ", "#    ", " #   ", "  #  "],
    "=": ["     ", "#####", "     ", "     ", "#####", "     "],
    ">": [" #   ", "  #  ", "   # ", "    #", "   # ", "  #  "],
    "?": [" ### ", "#   #", "   # ", "  #  ", "     ", "  #  "],
    "@": [" ### ", "#   #", "# ###", "# #.#", "#    ", " ### "],
    "A": ["  #  ", " # # ", "#   #", "#####", "#   #", "#   #"],
    "B": ["#### ", "#   #", "#### ", "#   #", "#   #", "#### "],
    "C": [" ### ", "#   #", "#    ", "#    ", "#   #", " ### "],
    "D": ["#### ", "#   #", "#   #", "#   #", "#   #", "#### "],
    "E": ["#####", "#    ", "#### ", "#    ", "#    ", "#####"],
    "F": ["#####", "#    ", "#### ", "#    ", "#    ", "#    "],
    "G": [" ### ", "#   #", "#    ", "#  ##", "#   #", " ### "],
    "H": ["#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "I": ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "#####"],
    "J": ["  ###", "   # ", "   # ", "   # ", "#  # ", " ##  "],
    "K": ["#   #", "#  # ", "# #  ", "##   ", "# #  ", "#   #"],
    "L": ["#    ", "#    ", "#    ", "#    ", "#    ", "#####"],
    "M": ["#   #", "## ##", "# # #", "#   #", "#   #", "#   #"],
    "N": ["#   #", "##  #", "# # #", "#  ##", "#   #", "#   #"],
    "O": [" ### ", "#   #", "#   #", "#   #", "#   #", " ### "],
    "P": ["#### ", "#   #", "#   #", "#### ", "#    ", "#    "],
    "Q": [" ### ", "#   #", "#   #", "#   #", "#  # ", " ## #"],
    "R": ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # "],
    "S": [" ### ", "#   #", " ##  ", "  ## ", "#   #", " ### "],
    "T": ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
    "U": ["#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    "V": ["#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
    "W": ["#   #", "#   #", "#   #", "# # #", "# # #", " # # "],
    "X": ["#   #", "#   #", " # # ", "  #  ", " # # ", "#   #"],
    "Y": ["#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  "],
    "Z": ["#####", "    #", "   # ", "  #  ", " #   ", "#####"],
}


def load_font(size):
    from PIL import ImageFont
    if not os.path.exists(CACHE):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        urllib.request.urlretrieve(FONT_URL, CACHE)
    return ImageFont.truetype(CACHE, size)


def render_glyphs(font):
    from PIL import Image, ImageDraw
    glyphs = []
    for code in range(FIRST, LAST + 1):
        img = Image.new("L", (CELL_W, CELL_H), 0)
        ImageDraw.Draw(img).text((0, 0), chr(code), font=font, fill=255)
        px = img.load()
        rows = []
        for y in range(CELL_H):
            bits = 0
            for x in range(CELL_W):
                if px[x, y] > 127:
                    bits |= 0x40 >> x
            rows.append(bits)
        glyphs.append(rows)
    return glyphs


def pixel_glyphs():
    glyphs = []
    for code in range(FIRST, LAST + 1):
        art = PIXEL[chr(code)]
        rows = [0] * CELL_H
        for r, line in enumerate(art):
            bits = 0
            for c, ch in enumerate(line):
                if ch == "#":
                    bits |= 0x40 >> c
            rows[PIXEL_TOP + r] = bits
        glyphs.append(rows)
    return glyphs


def header(glyphs, prefix, adv, top, note):
    up = prefix.upper()
    out = [
        note[0],
        note[1],
        "// (c) 2022-2024 Helena Zhang (helenazhang.com), SIL Open Font",
        "// License 1.1 (see tools/font/OFL.txt). Do not edit by hand.",
        "#pragma once",
        "",
        "#define %s_W   %d" % (up, CELL_W),
        "#define %s_H   %d" % (up, CELL_H),
        "#define %s_ADV %d" % (up, adv),
        "#define %s_TOP %d" % (up, top),
        "",
        "static const uint8_t %sFont[%d][%s_H] = {" % (prefix, LAST - FIRST + 1, up),
    ]
    for i, rows in enumerate(glyphs):
        ch = chr(FIRST + i)
        label = "space" if ch == " " else ch
        body = ",".join("0x%02X" % r for r in rows)
        out.append("  { %s }, // %s" % (body, label))
    out += ["};", ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=int, default=11)
    ap.add_argument("--pixel", action="store_true",
                    help="emit the hand-drawn 5x6 small set instead of rasterizing")
    ap.add_argument("--prefix", default="dm")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    if args.pixel:
        text = header(
            pixel_glyphs(), args.prefix, 6, 2,
            ("// Departure Mono small labels: hand-drawn 5x6 pixel glyphs by",
             "// tools/font/make_departure_font.py --pixel. In the Departure Mono"),
        )
        with open(args.out, "w") as f:
            f.write(text)
        print("wrote", args.out, "(pixel 5x6, prefix %s, adv 6)" % args.prefix)
        return
    font = load_font(args.size)
    text = header(
        render_glyphs(font), args.prefix,
        round(font.getlength("0")), font.getbbox("H")[1],
        ("// Departure Mono bitmap glyphs, rasterized by",
         "// tools/font/make_departure_font.py. Font: Departure Mono"),
    )
    with open(args.out, "w") as f:
        f.write(text)
    print("wrote", args.out, "(size %d, prefix %s, adv %d)" % (
        args.size, args.prefix, round(font.getlength("0"))))


if __name__ == "__main__":
    main()
