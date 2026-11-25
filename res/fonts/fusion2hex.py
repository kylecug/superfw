#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Converts Fusion Pixel Font BDF files (https://github.com/TakWolf/fusion-pixel-font,
# SIL Open Font License 1.1) into the .hex files that generator.py consumes.
#
# Two outputs are produced:
#  * ASCII glyphs (U+0020..U+007E) as 8 pixel wide, 16 row cells (32 hex digits).
#    generator.py trims the empty columns, so the font becomes proportional.
#  * CJK glyphs as 12 pixel wide, 16 row cells (64 hex digits, one 12 bit row each).
#
# The Fusion Pixel cell (12 rows for the monospaced font, 16 for the proportional
# one) is centered in the 16 pixel lines the menus are laid out with.
#
# Usage:
#  ./fusion2hex.py --ascii-bdf fusion-pixel-12px-proportional-zh_hans.bdf \
#                              fusion-pixel-12px-monospaced-zh_hans.bdf \
#      --cjk-bdf fusion-pixel-12px-monospaced-zh_hans.bdf \
#                fusion-pixel-12px-monospaced-zh_hant.bdf \
#                fusion-pixel-12px-monospaced-ja.bdf \
#      --out-ascii fusion-ascii.hex --out-cjk fusion-cjk12.hex
# The CJK fonts are tried in order, the first one that has the glyph is used.

import argparse, re


parser = argparse.ArgumentParser()
parser.add_argument('--ascii-bdf', nargs='+', required=True)
parser.add_argument('--cjk-bdf', nargs='+', required=True)
parser.add_argument('--out-ascii', required=True)
parser.add_argument('--out-cjk', required=True)
args = parser.parse_args()

CJK_RANGES = [
  (0x3000, 0x3009),   # CJK symbols and punctuation
  (0x3040, 0x30FF),   # Hiragana and katakana
  (0x4E00, 0x9FEF),   # Unified ideographs
  (0xFF01, 0xFF20),   # Full-width forms
]

def parse_bdf(path):
  glyphs, cur, inbm, ascent, descent = {}, None, False, 10, 2
  for line in open(path, encoding='utf-8'):
    t = line.split()
    if not t:
      continue
    if t[0] == 'FONT_ASCENT':
      ascent = int(t[1])
    elif t[0] == 'FONT_DESCENT':
      descent = int(t[1])
    elif t[0] == 'STARTCHAR':
      cur, inbm = {'bm': []}, False
    elif cur is None:
      continue
    elif t[0] == 'ENCODING':
      cur['enc'] = int(t[1])
    elif t[0] == 'BBX':
      cur['bbx'] = tuple(int(x) for x in t[1:5])
    elif t[0] == 'BITMAP':
      inbm = True
    elif t[0] == 'ENDCHAR':
      glyphs[cur['enc']] = cur
      cur, inbm = None, False
    elif inbm:
      cur['bm'].append(t[0])
  return glyphs, ascent, (16 - (ascent + descent)) // 2

def cell_rows(g, ascent, pad, cellw):
  """Returns 16 row values (cellw bits, leftmost pixel in the MSB) or None."""
  w, h, xo, yo = g['bbx']
  if xo < 0 or xo + w > cellw:
    return None
  top = pad + ascent - (yo + h)
  if top < 0 or top + h > 16:
    return None
  rows = [0] * 16
  for r, hexrow in enumerate(g['bm']):
    nbits = len(hexrow) * 4
    val = int(hexrow, 16)
    for c in range(w):
      if val & (1 << (nbits - 1 - c)):
        rows[top + r] |= 1 << (cellw - 1 - (xo + c))
  return rows

# ASCII
afonts = [parse_bdf(p) for p in args.ascii_bdf]
out = []
for cp in range(0x20, 0x7F):
  rows = None
  for gl, asc, pad in afonts:
    if cp in gl:
      rows = cell_rows(gl[cp], asc, pad, 8)
      if rows is not None:
        break
  if rows is None:
    continue
  out.append('%05X:%s' % (cp, ''.join('%02X' % r for r in rows)))
open(args.out_ascii, 'w').write('\n'.join(out) + '\n')
print('ASCII glyphs:', len(out))

# CJK, with fallbacks
fonts = [parse_bdf(p) for p in args.cjk_bdf]
out, missing = [], 0
for lo, hi in CJK_RANGES:
  for cp in range(lo, hi + 1):
    rows = None
    for gl, asc, pad in fonts:
      if cp in gl:
        rows = cell_rows(gl[cp], asc, pad, 12)
        if rows is not None:
          break
    if rows is None:
      missing += 1
      continue
    out.append('%05X:%s' % (cp, ''.join('%04X' % r for r in rows)))
open(args.out_cjk, 'w').write('\n'.join(out) + '\n')
print('CJK glyphs:', len(out), 'missing in all fonts:', missing)
