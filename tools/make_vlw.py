#!/usr/bin/env python3
"""Regenerate the VLW smooth-font headers used by menu.h.

The original generator was lost; this rebuilds it from the on-disk format. It keeps
every existing glyph byte-identical and only appends the codepoints that were missing,
so the layout that menu.h's hand-tuned pixel offsets depend on cannot shift.

VLW layout (all big-endian uint32):
  header : glyphCount, version, fontSize, mboxTop, ascent, descent
  glyph[]: codepoint, height, width, gxAdvance, dY, dX, reserved   (28 B each)
  bitmap : height*width bytes of 8-bit alpha per glyph, in glyph order

dY is the distance from the glyph's top edge down to the baseline; dX is a left
side-bearing (negative shifts left). Glyphs are sorted by codepoint -- TFT_eSPI walks
the table linearly, but keeping it ordered matches what the original produced.
"""
import re, struct, sys
from PIL import Image, ImageDraw, ImageFont

FONT_SRC = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"

# Latin-1 supplement (U+00A0-U+00FF minus the soft hyphen) covers German umlauts,
# French/Spanish/Nordic accents and the degree sign -- SpoolManagerExtended stores
# these fields as free utf8mb4 text with no whitelist, so anything can arrive, but
# Latin-1 is what realistically shows up in filament brand and colour names.
# Anything outside it renders as '?' via menuAsciiFallback() rather than the
# white box TFT_eSPI draws for a missing glyph.
#
# The original glyph set also skipped a chunk of printable ASCII (quotes, brackets,
# '_', '<'/'>', '{'/'}', '~', ...) -- fine until a spool name uses one, e.g. "tag_test",
# which then hit the same missing-glyph white box. Add the full printable ASCII range
# too so any punctuation in a name/vendor/material string renders instead of boxing.
#
# Only OctoFontMid gets these: OctoFontBig renders weights, tag types and UIDs
# ("842.3 g", "NFC-A", hex UIDs) -- never user text -- so the KB the same
# extension would cost there buys nothing.
ADD = ("".join(chr(c) for c in range(0x20, 0x7F)) +
       "".join(chr(c) for c in range(0xA0, 0x100) if c != 0xAD))
EXTEND_ONLY = {"OctoFontMid"}


def parse(path):
    src = open(path).read()
    by = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', src))
    n, ver, size, mbo, asc, desc = struct.unpack('>6I', by[:24])
    glyphs, off, bmp = [], 24, 24 + n * 28
    for i in range(n):
        cp, h, w, gx, dY = struct.unpack('>5I', by[off:off + 20])
        dX = struct.unpack('>i', by[off + 20:off + 24])[0]
        glyphs.append({'cp': cp, 'h': h, 'w': w, 'gx': gx, 'dY': dY, 'dX': dX,
                       'bmp': by[bmp:bmp + h * w]})
        bmp += h * w
        off += 28
    return {'ver': ver, 'size': size, 'mbo': mbo, 'asc': asc, 'desc': desc}, glyphs


def render(ch, ttf, ascent):
    """Rasterise one glyph to 8-bit alpha, matching the existing metrics convention."""
    mask = ttf.getmask(ch, mode='L')
    w, h = mask.size
    if w == 0 or h == 0:
        return {'h': 0, 'w': 0, 'gx': int(ttf.getlength(ch)), 'dY': 0, 'dX': 0, 'bmp': b''}
    bbox = ttf.getbbox(ch)          # (x0, y0, x1, y1) from the text origin (top-left)
    return {'h': h, 'w': w,
            'gx': int(round(ttf.getlength(ch))),
            'dY': ascent - bbox[1],  # top edge -> baseline
            'dX': bbox[0],
            'bmp': bytes(mask)}


def emit(path, name, hdr, glyphs):
    out = struct.pack('>6I', len(glyphs), hdr['ver'], hdr['size'],
                      hdr['mbo'], hdr['asc'], hdr['desc'])
    for g in glyphs:
        out += struct.pack('>5I', g['cp'], g['h'], g['w'], g['gx'], g['dY'])
        out += struct.pack('>i', g['dX']) + b'\x00\x00\x00\x00'
    for g in glyphs:
        out += g['bmp']
    body = "".join(
        "  " + ",".join(f"0x{b:02X}" for b in out[i:i + 16]) + ",\n"
        for i in range(0, len(out), 16))
    open(path, "w").write(
        "// Auto-generiert (make_vlw.py). Smooth-Font (VLW) als PROGMEM-Array.\n"
        "#pragma once\n"
        f"const uint8_t {name}[] PROGMEM = {{\n{body}}};\n")
    return len(out)


for name in ("OctoFontMid", "OctoFontBig"):
    path = f"src/{name}.h"
    hdr, glyphs = parse(path)
    if name not in EXTEND_ONLY:
        print(f"{name}: unveraendert ({len(glyphs)} Glyphen, kein Nutzertext)")
        continue
    have = {g['cp'] for g in glyphs}
    ttf = ImageFont.truetype(FONT_SRC, hdr['size'])
    added = []
    for ch in ADD:
        if ord(ch) in have:
            continue
        g = render(ch, ttf, hdr['asc'])
        g['cp'] = ord(ch)
        glyphs.append(g)
        added.append(ch)
    glyphs.sort(key=lambda g: g['cp'])
    size = emit(path, name, hdr, glyphs)
    print(f"{name}: +{len(added)} Glyphen ({''.join(added)}) -> {len(glyphs)} total, {size} B")
