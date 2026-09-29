"""
diff_unicorn.py - Confronto differenziale del modulo CPU con Unicorn (QEMU).

Genera istruzioni casuali di elaborazione dati e moltiplicazione (quelle con
semantica identica tra ARMv2 e ARM moderni: niente R15, niente MRS/MSR),
le esegue su Unicorn e sull'emulatore, e confronta registri e flag NZCV.

    .venv\\Scripts\\python tests\\diff_unicorn.py [casi] [seme]
"""
import random
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn import arm_const as A

# le costanti di Unicorn non sono consecutive: R13/R14 hanno codici a parte
REGS = [getattr(A, f"UC_ARM_REG_R{k}") for k in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]
UC_ARM_REG_CPSR = A.UC_ARM_REG_CPSR

ROOT = Path(__file__).resolve().parent.parent
ORACLE = next((p for p in [ROOT / "build/Release/arm2_oracle.exe",
                           ROOT / "build/Debug/arm2_oracle.exe",
                           ROOT / "build/arm2_oracle"] if p.exists()), None)

CODE = 0x100000
CODE_SIZE = 0x400000


def interesting_value(rng):
    return rng.choice([
        0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33,
        rng.getrandbits(8), rng.getrandbits(32), rng.getrandbits(32),
        1 << rng.randrange(32),
    ])


def random_instr(rng):
    cond = rng.choice([0xE] * 6 + list(range(15)))
    reg = lambda: rng.randrange(15)
    if rng.random() < 0.12:                                  # MUL / MLA
        rd, rm = reg(), reg()
        while rm == rd:
            rm = reg()
        a, s = rng.randrange(2), rng.randrange(2)
        rn = reg() if a else 0                               # MUL: Rn "should be zero"
        return (cond << 28) | (a << 21) | (s << 20) | (rd << 16) | (rn << 12) \
            | (reg() << 8) | 0x90 | rm
    op = rng.randrange(16)
    s = 1 if 8 <= op <= 11 else rng.randrange(2)            # TST..CMN senza S = MRS/MSR
    # campi "should be zero" che l'ARM2 ignora ma Unicorn rifiuta
    rn = 0 if op in (0xD, 0xF) else reg()
    rd = 0 if 8 <= op <= 11 else reg()
    i = cond << 28 | op << 21 | s << 20 | rn << 16 | rd << 12
    kind = rng.randrange(3)
    if kind == 0:                                            # immediato ruotato
        return i | 1 << 25 | rng.randrange(16) << 8 | rng.getrandbits(8)
    if kind == 1:                                            # shift immediato
        return i | rng.randrange(32) << 7 | rng.randrange(4) << 5 | reg()
    return i | reg() << 8 | rng.randrange(4) << 5 | 0x10 | reg()   # shift da registro


def run_unicorn(uc, addr, instr, nzcv, regs):
    # ogni caso a un indirizzo nuovo: Unicorn tiene in cache i blocchi tradotti
    uc.mem_write(addr, instr.to_bytes(4, "little"))
    for k, v in enumerate(regs):
        uc.reg_write(REGS[k], v)
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    uc.reg_write(UC_ARM_REG_CPSR, (cpsr & 0x0FFFFFFF) | (nzcv << 28))
    uc.emu_start(addr, addr + 4)
    return (uc.reg_read(UC_ARM_REG_CPSR) >> 28,
            [uc.reg_read(r) for r in REGS])


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    if ORACLE is None:
        sys.exit("arm2_oracle non trovato: compilare prima (cmake --build build --config Release)")
    rng = random.Random(seed)
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(CODE, CODE_SIZE)

    cases = []
    for _ in range(count):
        instr = random_instr(rng)
        nzcv = rng.randrange(16)
        regs = [interesting_value(rng) for _ in range(15)]
        cases.append((instr, nzcv, regs))

    stdin = "".join(f"{i:x} {f:x} " + " ".join(f"{r:x}" for r in regs) + "\n"
                    for i, f, regs in cases)
    out = subprocess.run([str(ORACLE)], input=stdin, capture_output=True, text=True, check=True)
    lines = out.stdout.splitlines()
    assert len(lines) == count, f"l'oracolo ha risposto {len(lines)} righe su {count}"

    failures = 0
    for k, ((instr, nzcv, regs), line) in enumerate(zip(cases, lines)):
        got = [int(x, 16) for x in line.split()]
        exp_f, exp_r = run_unicorn(uc, CODE + 4 * (k % (CODE_SIZE // 4)), instr, nzcv, regs)
        if got[0] != exp_f or got[1:] != exp_r:
            failures += 1
            if failures <= 15:
                print(f"DIVERSO {instr:08X} nzcv={nzcv:04b}")
                print(f"   unicorn nzcv={exp_f:04b} " + " ".join(f"{r:08X}" for r in exp_r))
                print(f"   arm2    nzcv={got[0]:04b} " + " ".join(f"{r:08X}" for r in got[1:]))
    print(f"{count} istruzioni casuali, {failures} differenze")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
