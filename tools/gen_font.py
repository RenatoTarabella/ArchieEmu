"""gen_font.py - genera src/riscos/font.h dal font di sistema di RISC OS
(Kernel/s/vdu/vdufontl1, licenza Apache 2.0): 224 caratteri 8x8, codici 32-255.

    python tools/gen_font.py vdufontl1 > src/riscos/font.h
"""
import re, sys

rows = []
for line in open(sys.argv[1], encoding="latin-1"):
    m = re.match(r"\s*=\s*((?:&[0-9A-Fa-f]{2}\s*,\s*){7}&[0-9A-Fa-f]{2})", line)
    if m:
        rows.append([int(v.strip()[1:], 16) for v in m.group(1).split(",")])
assert len(rows) == 224, len(rows)
print("/* font.h - font di sistema di RISC OS (Kernel/s/vdu/vdufontl1)")
print(" * Copyright Acorn Computers Ltd, licenza Apache 2.0. Generato da tools/gen_font.py */")
print("#ifndef RISCOS_FONT_H\n#define RISCOS_FONT_H\n")
print("/* righe dall'alto, bit 7 = pixel a sinistra; indice = codice - 32 */")
print("static const unsigned char riscos_font[224][8] = {")
for i, r in enumerate(rows):
    print("    { " + ", ".join(f"0x{v:02X}" for v in r) + f" }},  /* {i + 32} */")
print("};\n\n#endif")
