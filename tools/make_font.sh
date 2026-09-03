#!/usr/bin/env bash
#
# Regenerates components/wfc_font/src/wfc_font_16.c.
#
# You only need this when the character set changes. The generated file is
# committed, so an ordinary build never runs it -- it needs node and an 8 MB
# font download, and neither belongs in a firmware build.
#
# What goes in, and why:
#
#   ASCII 0x20-0x7F         Latin, digits, punctuation
#   GB2312 level 1          the 3755 characters that cover essentially all
#                           modern simplified Chinese. Level 2 is another 3008
#                           rare ones -- twice the flash for text nobody sends
#   CJK punctuation         ，。？！ and friends, which are NOT in GB2312's
#                           level 1 block and are in every sentence
#   a few geometric marks   ● ← → ✓, drawn as text rather than as widgets
#   FontAwesome, LVGL's set the LV_SYMBOL_* codepoints. The keyboard widget's
#                           own key map is built from them, so leaving them out
#                           breaks the keyboard, not just the decoration
#
# 4 bpp rather than 2: a 16 px Chinese glyph has strokes one pixel wide, and
# 2 bpp antialiasing makes a wall of them look like grey noise. It doubles the
# font to ~480 KB, which the 5 MB app partition can afford.
#
# Font: Noto Sans SC, SIL Open Font License 1.1 -- see the component's README.

set -euo pipefail

# The project this script lives in, not the working directory: the same file is
# checked out in the development tree and in the published one.
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
out="$root/components/wfc_font/src/wfc_font_16.c"
lvgl="$root/managed_components/lvgl__lvgl"
work="${TMPDIR:-/tmp}/wfc-font"

fa="$lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
otf_url=https://github.com/googlefonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf

mkdir -p "$work"
cd "$work"

[ -f NotoSansSC-Regular.otf ] || curl -fsSL -o NotoSansSC-Regular.otf "$otf_url"
[ -d node_modules/lv_font_conv ] || npm install --silent --no-audit --no-fund lv_font_conv

python3 - <<'PY'
chars = []
for hi in range(0xB0, 0xD8):                 # GB2312 level 1
    for lo in range(0xA1, 0xFF):
        try:
            chars.append(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
punct = "，。、；：？！“”‘’（）〈〉《》【】…—～·　％＃＠￥"
marks = "●○◆■□▲▼←→↑↓✓✕‹›«»"
open("charset.txt", "w", encoding="utf-8").write("".join(chars) + punct + marks)
print(f"{len(set(''.join(chars) + punct + marks))} characters")
PY

# The LV_SYMBOL_* codepoints, taken from the built-in font's own recipe so the
# set stays exactly the one LVGL's widgets expect.
symbols=$(head -6 "$lvgl/src/font/lv_font_source_han_sans_sc_16_cjk.c" |
          grep -o '61441[0-9,]*')

./node_modules/.bin/lv_font_conv \
    --no-compress --no-prefilter --bpp 4 --size 16 \
    --font NotoSansSC-Regular.otf -r 0x20-0x7F --symbols "$(cat charset.txt)" \
    --font "$fa" -r "$symbols" \
    --format lvgl --force-fast-kern-format --lv-include lvgl.h \
    -o wfc_font_16.c

cp wfc_font_16.c "$out"
echo "wrote $out ($(wc -c < "$out") bytes)"
