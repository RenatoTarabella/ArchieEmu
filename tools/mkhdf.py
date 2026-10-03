"""
mkhdf.py - Crea un'immagine di disco fisso IDE per il Risc PC (.hdf) gia'
formattata ADFS, come la lascia HForm 2.23 ("I", inizializza) su RISC OS 3.5.

    python tools/mkhdf.py disco.hdf MB [--name NomeDisco]
    python tools/mkhdf.py --check riferimento.hdf     (confronto con HForm)

Geometria come la propone l'emulatore (16 testine, 63 settori, settori da
512 byte, cilindri = MB * 2048 / 1008). Il disco e' "new map":
  - la mappa e' fatta di nzones zone da un settore (4096 bit); ogni zona
    ha 4 byte d'intestazione (ZoneCheck, FreeLink, CrossCheck), poi
    zone_bits bit d'allocazione (bit 32 .. 32+zone_bits-1), poi i bit di
    riserva fino a 4095; la zona 0 tiene anche una copia del record del
    disco (60 byte, bit 32-511);
  - il bit b della zona z vale il disco da (z * zone_bits + b - 32 - 480)
    unita' da 2^log2bpmb byte;
  - un frammento e' un id di idlen bit seguito da zeri, chiuso da un bit 1;
    nei frammenti liberi l'id e' la distanza dal prossimo libero (0 =
    ultimo) e FreeLink (bit 15 a 1) e' la distanza dal bit 8 al primo;
  - l'oggetto 2 (di sistema) ha un frammento all'inizio del disco (blocco
    di boot a &C00) e uno all'inizio della zona centrale (nzones // 2) con
    le due copie della mappa e la directory radice ("Hugo", 2 KB);
  - i bit di riserva e quelli oltre la fine del disco vanno all'oggetto 1.
I parametri (idlen, log2bpmb, nzones, zone_spare) sono quelli scelti da
HForm per ogni dimensione; per le altre si sceglie con la stessa regola.
"""
import argparse
import random
import struct
import sys

SECSIZE = 512
HEADS, SPT = 16, 63
DIR_SIZE = 2048


def geometry(mb):
    cyls = mb * 2048 // (HEADS * SPT)
    return cyls, cyls * HEADS * SPT * SECSIZE


def choose_params(disc_size):
    """idlen, log2bpmb, nzones, zone_spare: i valori di HForm 2.23.
    Si cerca il piu' piccolo log2bpmb (da 9) e idlen (da 13) con cui gli id
    bastano per tutti i frammenti possibili, come fa FileCore."""
    for log2bpmb in range(9, 13):
        bits = disc_size >> log2bpmb
        for idlen in range(13, 16):
            zone_bits_max = SECSIZE * 8 - 32
            nzones = None
            for spare in range(32, 200):
                zb = SECSIZE * 8 - spare
                nz = (bits + 480 + zb - 1) // zb
                # in ogni zona ci devono stare al piu' 2^idlen frammenti
                if zb // (idlen + 1) * nz <= (1 << idlen) and nz <= 127:
                    nzones = nz
                    # HForm distribuisce lo spazio: le riserve sono quelle che
                    # avanzano dividendo i bit fra le zone
                    zb = (bits + 480 + nz - 1) // nz
                    return idlen, log2bpmb, nz, SECSIZE * 8 - zb
    raise ValueError("disco troppo grande")


# parametri osservati con HForm 2.23 (MB -> idlen, log2bpmb, nzones, zone_spare)
HFORM = {64: (14, 9, 33, 97), 100: (14, 9, 51, 74), 128: (15, 9, 65, 49),
         256: (15, 10, 65, 49), 512: (15, 11, 65, 49)}


class Bits:
    def __init__(self, data):
        self.d = data

    def set(self, bit, value=1):
        if value:
            self.d[bit >> 3] |= 1 << (bit & 7)

    def put(self, bit, nbits, value):
        for k in range(nbits):
            if (value >> k) & 1:
                self.set(bit + k)


def zone_check(m):
    s0 = s1 = s2 = s3 = 0
    for i in range(SECSIZE - 4, 0, -4):
        s0 += m[i] + (s3 >> 8); s3 &= 0xFF
        s1 += m[i + 1] + (s0 >> 8); s0 &= 0xFF
        s2 += m[i + 2] + (s1 >> 8); s1 &= 0xFF
        s3 += m[i + 3] + (s2 >> 8); s2 &= 0xFF
    s0 += s3 >> 8
    s1 += m[1] + (s0 >> 8)
    s2 += m[2] + (s1 >> 8)
    s3 += m[3] + (s2 >> 8)
    return (s0 ^ s1 ^ s2 ^ s3) & 0xFF


def boot_check(b):
    c = 0
    for i in range(0x1FE, -1, -1):
        c = (c & 0xFF) + (c >> 8) + b[i]
    return c & 0xFF


def ror(x, n):
    return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF


def dir_check(d, entries_end=5):
    """byte di controllo di una directory Hugo (come FileCore, vedi mkadfs.py);
    in una directory vuota le voci finiscono dopo i 5 byte d'intestazione"""
    acc = 0
    p = 0
    while p < entries_end & ~3:
        acc = struct.unpack_from("<I", d, p)[0] ^ ror(acc, 13)
        p += 4
    while p < entries_end:
        acc = d[p] ^ ror(acc, 13)
        p += 1
    p = len(d) - 40
    while p < len(d) - 4:
        acc = struct.unpack_from("<I", d, p)[0] ^ ror(acc, 13)
        p += 4
    acc ^= acc >> 16
    acc ^= acc >> 8
    return acc & 0xFF


def disc_record(p, disc_size, root, disc_id, name, for_map):
    idlen, log2bpmb, nzones, spare = p
    r = bytearray(60)
    struct.pack_into('<BBBBBBBBBBHIIH', r, 0, 9, SPT, HEADS, 0, idlen, log2bpmb, 0, 0, 1, nzones,
                     spare, root, disc_size, disc_id if for_map else 0)
    if for_map:
        n = name.encode('latin-1')[:10]
        r[22:22 + len(n)] = n
        if len(n) < 10:
            r[22 + len(n)] = 0x0D
        r[34] = 0x04                         # come HForm: byte 34 = 4 nella copia della mappa
    return r


def build(mb, name="IDEDisc4", disc_id=None, params=None):
    cyls, disc_size = geometry(mb)
    p = params or HFORM.get(mb) or choose_params(disc_size)
    idlen, log2bpmb, nzones, spare = p
    zone_bits = SECSIZE * 8 - spare
    bpmb = 1 << log2bpmb
    if disc_id is None:
        disc_id = random.randrange(0x10000)
    root = 0x200 | (2 * nzones + 1)
    img = bytearray(mb << 20)                    # il file: MB interi, il disco un po' meno

    def zone_bit(global_bit):
        g = global_bit + 480
        return g // zone_bits, g % zone_bits + 32

    total_bits = disc_size >> log2bpmb
    mapzone = nzones // 2
    sys_len = (2 * nzones * SECSIZE + DIR_SIZE + bpmb - 1) // bpmb
    sys_len = max(sys_len, idlen + 1)
    boot_len = max(idlen + 1, (0xE00 + bpmb - 1) // bpmb)

    zones = []
    for z in range(nzones):
        m = bytearray(SECSIZE)
        bits = Bits(m)
        first = 32 + (480 if z == 0 else 0)
        last = 32 + zone_bits                    # primo bit di riserva
        # fine del disco in questa zona
        end_g = total_bits + 480
        zone_end = min(last, end_g - z * zone_bits + 32) if end_g - z * zone_bits < zone_bits else last
        used = []                                # (inizio, lunghezza, id)
        if z == 0:
            used.append((first, boot_len, 2))
        if z == mapzone:
            used.append((32, sys_len, 2))
        pos = first
        for s, n, i in used:
            if s == pos:
                pos = s + n
        frags = []
        for s, n, i in used:
            bits.put(s, idlen, i)
            bits.set(s + n - 1)
        free_start = pos
        if free_start < zone_end:
            bits.set(zone_end - 1)               # un solo frammento libero, id = 0 (ultimo)
            freelink = 0x8000 | (free_start - 8)
        else:
            freelink = 0x8000
        # riserva e oltre la fine del disco: oggetto 1
        bits.put(zone_end, idlen, 1)
        bits.set(SECSIZE * 8 - 1)
        m[1] = freelink & 0xFF
        m[2] = freelink >> 8
        if z == 0:
            m[4:64] = disc_record(p, disc_size, root, disc_id, name, True)
        zones.append(m)

    for z, m in enumerate(zones):
        m[3] = 0xFF if z == 0 else 0
        m[0] = zone_check(m)

    map_addr = (mapzone * zone_bits - 480) * bpmb
    for copy in range(2):
        for z, m in enumerate(zones):
            off = map_addr + (copy * nzones + z) * SECSIZE
            img[off:off + SECSIZE] = m
    # radice
    d = bytearray(DIR_SIZE)
    d[1:5] = b'Hugo'
    tail = DIR_SIZE - 41
    struct.pack_into('<I', d, tail + 3, root)          # dirparent (3 byte)
    d[tail + 6] = ord('$')                             # dirtitle
    d[tail + 25] = ord('$')                            # dirname
    d[DIR_SIZE - 5:DIR_SIZE - 1] = b'Hugo'
    d[DIR_SIZE - 1] = dir_check(d)
    roff = map_addr + 2 * nzones * SECSIZE
    img[roff:roff + DIR_SIZE] = d
    # blocco di boot
    bb = bytearray(SECSIZE)
    struct.pack_into('<I', bb, 0, 0x20000000)          # fine della lista dei difetti
    # parametri dell'unita' scritti da HForm per l'IDE: &1AC-&1AF a &FF, flag
    # di inizializzazione, indirizzo del cilindro di parcheggio (l'ultimo)
    bb[0x1AC:0x1B0] = bytes([0xFF] * 4)
    bb[0x1BB] = 1
    struct.pack_into('<I', bb, 0x1BC, (cyls - 1) * HEADS * SPT * SECSIZE)
    bb[0x1C0:0x1C0 + 60] = disc_record(p, disc_size, root, disc_id, name, False)
    bb[0x1FF] = boot_check(bb)
    img[0xC00:0xE00] = bb
    return img


def nonzero_sectors(b):
    zero = bytes(SECSIZE)
    return {i for i in range(0, len(b), SECSIZE) if b[i:i + SECSIZE] != zero}


def compare(path):
    ref = open(path, 'rb').read()
    mb = len(ref) >> 20
    p = struct.unpack_from('<BBBBBBBBBBH', ref, 0xC00 + 0x1C0)
    params = (p[4], p[5], p[9], p[10])
    nz, zs = p[9], p[10]
    zb = SECSIZE * 8 - zs
    map_addr = ((nz // 2) * zb - 480) << p[5]
    disc_id = struct.unpack_from('<H', ref, map_addr + 4 + 20)[0]
    name = bytes(ref[map_addr + 4 + 22:map_addr + 4 + 32]).split(bytes([13]))[0].split(bytes([0]))[0].decode('latin-1')
    img = build(mb, name, disc_id, params)
    if len(ref) != len(img):
        print(path, "dimensione diversa", len(ref), len(img))
        return False
    secs = sorted(nonzero_sectors(ref) | nonzero_sectors(img))
    diffs = []
    for s in secs:
        for i in range(s, s + SECSIZE):
            if ref[i] != img[i]:
                diffs.append(i)
    print("%s: %d MB, parametri %s (scelti: %s), %d settori usati, %d byte diversi"
          % (path, mb, params, choose_params(geometry(mb)[1]), len(secs), len(diffs)))
    for i in diffs[:24]:
        print("   %08X  HForm %02X  mkhdf %02X" % (i, ref[i], img[i]))
    return not diffs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('image', nargs='?')
    ap.add_argument('mb', nargs='?', type=int)
    ap.add_argument('--name', default='HardDisc4')
    ap.add_argument('--check', nargs='+')
    a = ap.parse_args()
    if a.check:
        ok = all([compare(p) for p in a.check])
        sys.exit(0 if ok else 1)
    if not a.image or not a.mb:
        ap.error("servono immagine e MB")
    open(a.image, 'wb').write(build(a.mb, a.name))
    print("%s: %d MB, ADFS new map" % (a.image, a.mb))


if __name__ == '__main__':
    main()
