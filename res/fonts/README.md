
unscii-16.hex and unscii-16-full.hex

Downloaded from http://viznut.fi/unscii/ under the GPL license

hangul-blocks.hex is also part of unifont, manually filtered and
re-numbered to work with hangul.h

The font pack has been generated using:

./generator.py  --font-files unscii-16-full.hex hangul-blocks.hex \
  --font-blocks cjk-sym,latin,latin-a,latin-b,greek,cyrilic,hiragana,katakana,cjk-uni,hangul-part \
  --output ../fonts.pack

A full-hangul version (no char composition) can be generated using:

./generator.py  --font-files unscii-16-full.hex hangul-blocks.hex \
  --font-blocks cjk-sym,latin,latin-a,latin-b,greek,cyrilic,hiragana,katakana,cjk-uni,hangul \
  --output ../fonts-ext.pack

The full font pack (for debugging purposes) can be generated using:

./generator.py --font-files unscii-16-full.hex \
  --font-blocks ascii,check,arrows,arrows2,cjk-sym,latin,latin-a,latin-b,greek,cyrilic,hiragana,katakana,cjk-uni,hangul \
  --output ../fonts-full.pack



Fusion Pixel (12px) variant
---------------------------

This fork uses Fusion Pixel Font 12px (https://github.com/TakWolf/fusion-pixel-font,
SIL Open Font License 1.1, see LICENSE-fusion-pixel-OFL.txt) for the ASCII and
CJK glyphs. CJK characters are 12 pixels wide instead of 16 (flag bit1 in the
block index), latin text is proportional. The other blocks (accented latin,
greek, cyrillic, hangul) still come from unscii/unifont.

Download the BDF releases (12px proportional and monospaced) and convert them:

./fusion2hex.py \
  --ascii-bdf fusion-pixel-12px-proportional-zh_hans.bdf fusion-pixel-12px-monospaced-zh_hans.bdf \
  --cjk-bdf fusion-pixel-12px-monospaced-zh_hans.bdf fusion-pixel-12px-monospaced-zh_hant.bdf \
            fusion-pixel-12px-monospaced-ja.bdf \
  --out-ascii fusion-ascii.hex --out-cjk fusion-cjk12.hex

The font pack is then generated with:

./generator.py --font-files unscii-16-full.hex hangul-blocks.hex --font-files-12 fusion-cjk12.hex \
  --font-blocks cjk-sym12,latin,latin-a,latin-b,greek,cyrilic,hiragana12,katakana12,cjk-uni12,fixwidth12,hangul-part \
  --output ../fonts.pack

And the embedded ASCII font (src/fonts/font_embed.h) with:

./generator.py --font-files unscii-16-full.hex fusion-ascii.hex \
  --font-blocks ascii,check,arrows,arrows2 --output /tmp/ascii.font

(The original unscii-only commands above still reproduce the original files.)
