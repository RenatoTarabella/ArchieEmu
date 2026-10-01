"""
gen_hostfs_module.py - Assembla il modulo HostFS e lo scrive come array C.

    .venv\\Scripts\\python tools\\gen_hostfs_module.py

Legge src/archie/hostfs_module.s, lo assembla con Keystone all'indirizzo 0
(le etichette diventano offset dall'inizio del modulo) e scrive
src/archie/hostfs_module.h. Il .h e' nel repository: per compilare
l'emulatore non servono ne' Python ne' Keystone.
"""
import os
import re
import tempfile
from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KsError

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "archie", "hostfs_module.s")
OUT = os.path.join(ROOT, "src", "archie", "hostfs_module.h")


def main():
    text = open(SRC, encoding="utf-8").read()
    # Keystone non conosce '@' come commento ovunque: si tolgono i commenti
    lines = []
    for line in text.splitlines():
        if '"' not in line:
            line = line.split("@", 1)[0]
        else:
            # commento dopo una stringa
            m = re.match(r'^(.*"[^"]*")\s*@.*$', line)
            if m:
                line = m.group(1)
        lines.append(line)
    try:
        code, _ = Ks(KS_ARCH_ARM, KS_MODE_ARM).asm("\n".join(lines), 0)
    except KsError as e:
        raise SystemExit(f"{SRC}: {e}")
    data = bytes(code)
    while len(data) % 4:
        data += b"\0"
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* hostfs_module.h - generato da tools/gen_hostfs_module.py: non modificare */\n")
        f.write(f"static const unsigned char hostfs_module[{len(data)}] = {{\n")
        for i in range(0, len(data), 16):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",\n")
        f.write("};\n")
    print(f"{OUT}: {len(data)} byte")


if __name__ == "__main__":
    main()
