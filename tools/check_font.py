#!/usr/bin/env python3
"""Which Chinese characters can this firmware actually draw?

The font is a fixed subset -- ASCII plus GB2312 level 1, see
components/wfc_font/ -- and a character outside it renders as nothing at all:
no box, no question mark, just a gap. So a label can be wrong on the panel
while being perfectly correct in the serial log, and nobody notices until the
board is on someone's desk. That is how P4 shipped a tab named 会话 that drew
as 会.

This reads the character set straight out of the generated font file (the
generator records its own --symbols argument at the top) and checks every
string literal in the sources against it.

    python3 tools/check_font.py main/*.c

Exits non-zero if anything in a literal cannot be drawn, so it can go in front
of a build. `--list` dumps the whole drawable set instead.
"""

import re
import sys
from pathlib import Path

# Relative to this script rather than to the working directory: the same file
# is checked out in the development tree and in the published one, under
# different names, and it gets run from either.
FONT = Path(__file__).resolve().parent.parent / "components/wfc_font/src/wfc_font_16.c"


def font_charset(path=FONT):
    """The characters the font holds: ASCII plus whatever --symbols listed."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        head = fh.read(8192)
    m = re.search(r"--symbols (.*?) --font ", head, re.S)
    if not m:
        sys.exit(f"{path}: no --symbols in the header comment")
    return set(m.group(1)) | {chr(c) for c in range(0x20, 0x80)}


def literals(path):
    """(line number, string) for every double-quoted literal in a C file."""
    out = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for n, line in enumerate(fh, 1):
            if line.lstrip().startswith(("*", "//")):
                continue          # a comment, not something that gets drawn
            for s in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
                if any(ord(c) > 0x7F for c in s):
                    out.append((n, s))
    return out


def main(argv):
    charset = font_charset()
    if "--list" in argv:
        cjk = sorted(c for c in charset if "一" <= c <= "鿿")
        print(f"{len(cjk)} CJK characters:\n" + "".join(cjk))
        return 0

    bad = 0
    for path in argv:
        for line, text in literals(path):
            missing = sorted({c for c in text
                              if ord(c) > 0x7F and c not in charset})
            if missing:
                bad += 1
                print(f'{path}:{line}: cannot draw {"".join(missing)}  in "{text}"')

    if bad:
        print(f"\n{bad} literal(s) the panel would draw with gaps in them.")
        return 1
    print(f"all literals drawable ({len(charset)} glyphs in the font)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
