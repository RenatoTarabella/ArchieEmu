"""
diff_unicorn_arm6.py - Confronto differenziale del core ARMv3 (src/cpu/arm6.c)
con Unicorn (QEMU, modello StrongARM SA1100) nei modi a 32 bit.

Istruzioni casuali eseguite una alla volta in uno dei sei modi a 32 bit:
elaborazione dati (anche con R15 come operando o destinazione), MUL/MLA, MRS, MSR, LDR/STR, LDM/STM, SWP. Si confrontano
CPSR, SPSR, PC, R0-R14 e le parole di memoria cambiate.

Restano fuori le differenze vere tra ARMv3 e ARMv4: LDR/STR non allineate
(l'ARM610 ruota la parola, QEMU no), STR PC (PC+12 contro PC+8), shift da
registro con R15, la base nella lista di un LDM/STM con write-back; e
MOVS PC, che Unicorn eseguito un'istruzione alla volta non completa. Quelle
sono nei test unitari (tests/test_arm6.c).

    .venv\\Scripts\\python tests\\diff_unicorn_arm6.py [casi] [seme]
"""
import random
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM
from unicorn import arm_const as A

REGS = [getattr(A, f"UC_ARM_REG_R{k}") for k in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]

ROOT = Path(__file__).resolve().parent.parent
ORACLE = next((p for p in [ROOT / "build/Release/arm6_oracle.exe",
                           ROOT / "build/Debug/arm6_oracle.exe",
                           ROOT / "build/arm6_oracle"] if p.exists()), None)

CODE = 0x100000
CODE_SIZE = 0x400000
DATA = 0x20000
DATA_SIZE = 0x1000
MODES = [0x10, 0x11, 0x12, 0x13, 0x17, 0x1B]
PSR_BITS = 0xF00000DF


def pattern_bytes():
    out = bytearray()
    for k in range(DATA_SIZE // 4):
        out += ((k * 0x9E3779B1 + 0x12345678) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(out)


PATTERN = pattern_bytes()


def interesting_value(rng):
    return rng.choice([
        0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33,
        rng.getrandbits(8), rng.getrandbits(32), rng.getrandbits(32),
        1 << rng.randrange(32),
    ])


def random_psr(rng):
    return rng.randrange(16) << 28 | rng.choice([0, 0x40, 0x80, 0xC0]) | rng.choice(MODES)


def gen_dp(rng, cond, regs, priv):
    op = rng.randrange(16)
    s = 1 if 8 <= op <= 11 else rng.randrange(2)
    reg = lambda: rng.randrange(16 if rng.random() < 0.15 else 15)
    rn = 0 if op in (0xD, 0xF) else reg()
    rd = 0 if 8 <= op <= 11 else rng.randrange(15)
    if 8 > op or op > 11:
        if rng.random() < 0.06:
            # salto; senza S: Unicorn, fermandosi dopo un'istruzione, non fa
            # il ritorno dall'eccezione (SPSR -> CPSR), che e' nei test unitari
            rd, s = 15, 0
    i = cond << 28 | op << 21 | s << 20 | rn << 16 | rd << 12
    kind = rng.randrange(3)
    if kind == 0:
        return i | 1 << 25 | rng.randrange(16) << 8 | rng.getrandbits(8)
    if kind == 1:
        return i | rng.randrange(32) << 7 | rng.randrange(4) << 5 | reg()
    if rn == 15:
        i &= ~(15 << 16)                                    # niente R15 con lo shift da registro
    return i | rng.randrange(15) << 8 | rng.randrange(4) << 5 | 0x10 | rng.randrange(15)


def gen_mul(rng, cond, regs, priv):
    reg = lambda: rng.randrange(15)
    rd, rm = reg(), reg()
    while rm == rd:
        rm = reg()
    a, s = rng.randrange(2), rng.randrange(2)
    rn = reg() if a else 0
    return cond << 28 | a << 21 | s << 20 | rd << 16 | rn << 12 | reg() << 8 | 0x90 | rm


def gen_mrs(rng, cond, regs, priv):
    spsr = rng.randrange(2) if priv else 0
    return cond << 28 | 0x010F0000 | spsr << 22 | rng.randrange(15) << 12


def gen_msr(rng, cond, regs, priv):
    spsr = rng.randrange(2) if priv else 0
    value = random_psr(rng)
    if rng.randrange(2):                                    # immediato: un campo solo
        if rng.randrange(2):
            return cond << 28 | 0x0320F000 | spsr << 22 | 1 << 16 | (value & 0xFF)
        return cond << 28 | 0x0320F000 | spsr << 22 | 8 << 16 | 2 << 8 | (value >> 28)
    rm = rng.randrange(15)
    regs[rm] = value
    fields = rng.choice([1, 8, 9])
    return cond << 28 | 0x0120F000 | spsr << 22 | fields << 16 | rm


def distinct(rng, avoid, top=15):
    while True:
        r = rng.randrange(top)
        if r not in avoid:
            return r


def gen_ldr_str(rng, cond, regs, priv):
    b, l = rng.randrange(2), rng.randrange(2)
    p, u, w = rng.randrange(2), rng.randrange(2), rng.randrange(2)
    align = 1 if b else 4
    rn = rng.randrange(15)
    regs[rn] = DATA + 0x800 + rng.randrange(-0x100, 0x100) * align
    writeback = (not p) or w
    rd = distinct(rng, {rn} if writeback else set())
    i = cond << 28 | 1 << 26 | p << 24 | u << 23 | b << 22 | w << 21 | l << 20 | rn << 16 | rd << 12
    if rng.randrange(2):
        return i | rng.randrange(0x100) * align
    rm = distinct(rng, {rn, rd})
    sh = rng.randrange(3)
    regs[rm] = rng.randrange(0x100 >> sh) * align
    return i | 1 << 25 | sh << 7 | rm


def gen_ldm_stm(rng, cond, regs, priv):
    p, u, w, l = rng.randrange(2), rng.randrange(2), rng.randrange(2), rng.randrange(2)
    rn = rng.randrange(15)
    regs[rn] = DATA + 0x800 + rng.randrange(-0x40, 0x40) * 4
    lst = rng.randrange(1, 0x8000)
    if w:
        lst &= ~(1 << rn)
        if not lst:
            lst = 1 << ((rn + 1) % 15)
    return cond << 28 | 0x08000000 | p << 24 | u << 23 | w << 21 | l << 20 | rn << 16 | lst


def gen_swp(rng, cond, regs, priv):
    b = rng.randrange(2)
    rn = rng.randrange(15)
    regs[rn] = DATA + 0x800 + rng.randrange(-0x100, 0x100) * (1 if b else 4)
    rd, rm = distinct(rng, {rn}), distinct(rng, {rn})
    return cond << 28 | 0x01000090 | b << 22 | rn << 16 | rd << 12 | rm


def gen_mull(rng, cond, regs, priv):
    """UMULL/UMLAL/SMULL/SMLAL (ARMv4): RdHi, RdLo e Rm diversi"""
    hi, lo, rm = rng.sample(range(15), 3)
    u, a, s = rng.randrange(2), rng.randrange(2), rng.randrange(2)
    return cond << 28 | 0x00800090 | u << 22 | a << 21 | s << 20 | hi << 16 | lo << 12 | rng.randrange(15) << 8 | rm


def gen_halfword(rng, cond, regs, priv):
    """LDRH/STRH/LDRSB/LDRSH (ARMv4); mezze parole allineate"""
    sh = rng.choice([1, 2, 3])
    l = 1 if sh != 1 else rng.randrange(2)
    p, u, w = rng.randrange(2), rng.randrange(2), rng.randrange(2)
    align = 1 if sh == 2 else 2
    rn = rng.randrange(15)
    regs[rn] = DATA + 0x800 + rng.randrange(-0x80, 0x80) * align
    writeback = (not p) or w
    rd = distinct(rng, {rn} if writeback else set())
    i = cond << 28 | p << 24 | u << 23 | w << 21 | l << 20 | rn << 16 | rd << 12 | 0x90 | sh << 5
    if rng.randrange(2):
        off = rng.randrange(0x80) * align
        return i | 1 << 22 | (off & 0xF0) << 4 | (off & 0xF)
    rm = distinct(rng, {rn, rd})
    regs[rm] = rng.randrange(0x80) * align
    return i | rm


GENERATORS = [(gen_dp, 35), (gen_mul, 8), (gen_mrs, 7), (gen_msr, 12),
              (gen_ldr_str, 20), (gen_ldm_stm, 13), (gen_swp, 5)]
V4 = "--v4" in sys.argv
if V4:
    GENERATORS += [(gen_mull, 15), (gen_halfword, 20)]


def make_case(rng):
    cpsr = random_psr(rng)
    spsr = random_psr(rng)
    regs = [interesting_value(rng) for _ in range(15)]
    cond = rng.choice([0xE] * 6 + list(range(15)))
    gen = rng.choices([g for g, _ in GENERATORS], [w for _, w in GENERATORS])[0]
    instr = gen(rng, cond, regs, (cpsr & 0x1F) != 0x10)
    return instr, cpsr, spsr, regs


def run_unicorn(uc, addr, instr, cpsr, spsr, regs):
    uc.mem_write(addr, instr.to_bytes(4, "little"))
    # dopo un cambio di modo QEMU riusa flag dei blocchi tradotti ormai vecchi
    uc.ctl_flush_tb()
    for m in MODES:                                         # tutti i banchi uguali
        uc.reg_write(A.UC_ARM_REG_CPSR, m | 0xC0)
        for k in range(8, 15):
            uc.reg_write(REGS[k], regs[k])
        if m != 0x10:
            uc.reg_write(A.UC_ARM_REG_SPSR, spsr)
    uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
    for k, v in enumerate(regs):
        uc.reg_write(REGS[k], v)
    try:
        uc.emu_start(addr, 0xFFFFFFFF, count=1)
    except UcError:
        pass                                                # salto verso memoria non mappata
    new_cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
    new_spsr = 0 if (new_cpsr & 0x1F) == 0x10 else uc.reg_read(A.UC_ARM_REG_SPSR)
    state = [new_cpsr & PSR_BITS, new_spsr & PSR_BITS, uc.reg_read(A.UC_ARM_REG_PC)]
    state += [uc.reg_read(r) for r in REGS]
    mem = bytes(uc.mem_read(DATA, DATA_SIZE))
    changed = []
    if mem != PATTERN:
        for k in range(0, DATA_SIZE, 4):
            if mem[k:k + 4] != PATTERN[k:k + 4]:
                changed.append((DATA + k, int.from_bytes(mem[k:k + 4], "little")))
        uc.mem_write(DATA, PATTERN)
    return state, changed


def parse_oracle(line):
    words = line.split()
    state = [int(x, 16) for x in words[:18]]
    state[0] &= PSR_BITS
    state[1] &= PSR_BITS
    changed = []
    for w in words[18:]:
        a, v = w.split("=")
        changed.append((int(a, 16), int(v, 16)))
    return state, changed


def main():
    args = [a for a in sys.argv[1:] if a != "--v4"]
    count = int(args[0]) if len(args) > 0 else 50000
    seed = int(args[1]) if len(args) > 1 else 1
    if ORACLE is None:
        sys.exit("arm6_oracle non trovato: compilare prima (cmake --build build --config Release)")
    rng = random.Random(seed)
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(A.UC_CPU_ARM_SA1100)
    uc.mem_map(CODE, CODE_SIZE)
    uc.mem_map(DATA, DATA_SIZE)
    uc.mem_write(DATA, PATTERN)

    cases = [make_case(rng) for _ in range(count)]
    addrs = [CODE + 4 * (k % (CODE_SIZE // 4 - 4)) for k in range(count)]
    stdin = "".join(f"{i:x} {a:x} {c:x} {s:x} " + " ".join(f"{r:x}" for r in regs) + "\n"
                    for (i, c, s, regs), a in zip(cases, addrs))
    out = subprocess.run([str(ORACLE)] + (["--v4"] if V4 else []), input=stdin, capture_output=True, text=True, check=True)
    lines = out.stdout.splitlines()
    assert len(lines) == count, f"l'oracolo ha risposto {len(lines)} righe su {count}"

    names = ["cpsr", "spsr", "pc"] + [f"r{k}" for k in range(15)]
    failures = 0
    for (instr, cpsr, spsr, regs), addr, line in zip(cases, addrs, lines):
        exp, exp_mem = run_unicorn(uc, addr, instr, cpsr, spsr, regs)
        got, got_mem = parse_oracle(line)
        if exp != got or exp_mem != got_mem:
            failures += 1
            if failures <= 15:
                print(f"DIVERSO {instr:08X} @{addr:X} cpsr={cpsr:08X} spsr={spsr:08X}")
                print("   ingresso " + " ".join(f"{r:08X}" for r in regs))
                for n, e, g in zip(names, exp, got):
                    if e != g:
                        print(f"   {n}: unicorn {e:08X}  arm6 {g:08X}")
                if exp_mem != got_mem:
                    print(f"   memoria: unicorn {exp_mem}  arm6 {got_mem}")
    print(f"{count} istruzioni casuali, {failures} differenze")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
