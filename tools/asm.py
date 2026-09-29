"""
asm.py - Assembla un sorgente ARM in un binario piatto per armemu (Keystone).

    .venv\\Scripts\\python tools\\asm.py sorgente.s uscita.bin [indirizzo]

Keystone conosce l'ARM moderno: usare solo istruzioni ARMv2 (niente MRS/MSR,
halfword, BX, UMULL...). Le SWI si scrivono "svc #n". Strumento provvisorio
finche' non ci sara' l'assembler del BBC BASIC.
"""
import sys
from keystone import Ks, KS_ARCH_ARM, KS_MODE_ARM, KsError



def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x8000
    text = open(src, encoding="utf-8").read()
    try:
        code, _ = Ks(KS_ARCH_ARM, KS_MODE_ARM).asm(text, base)
    except KsError as e:
        sys.exit(f"{src}: {e}")
    open(out, "wb").write(bytes(code))
    print(f"{out}: {len(code)} byte a &{base:X}")


if __name__ == "__main__":
    main()
