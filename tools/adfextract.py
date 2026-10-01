"""
adfextract.py - Estrae un'immagine floppy ADFS, un archivio RISC OS (zip,
Spark/Arc, ArcFS) o un CD-ROM in una cartella dell'host, con i nomi e i tipi
di RISC OS nella convenzione di HostFS/RPCEmu.

    python tools/adfextract.py immagine.adf|archivio|cd.iso[.zip] cartella [--list] [--keep-archives]

Esempio: tools/adfextract.py "Hopper 1.00.adf" HostFS/Hopper
Gli archivi si leggono con il codice di mkadfs.py, i CD con cdextract.py. Gli
archivi trovati dentro (tipi &3FB e &DDC) diventano cartelle, come in SparkFS.

Formati: L (640 KB, mappa vecchia, directory da &500), D (800 KB, mappa
vecchia, directory da &800) ed E (800 KB, mappa nuova a frammenti). Le
directory ("Hugo", o "Nick" nel formato E) hanno voci da 26 byte (nome 10, load, exec,
lunghezza, indirizzo su 3 byte, attributi). Nella mappa nuova l'indirizzo e'
"indiretto": numero del frammento (bit 8 e oltre) e settore dentro il
frammento + 1 (bit 0-7); la mappa e' una sequenza di frammenti, ognuno con
idlen bit di numero e una serie di zeri chiusa da un 1.

Sull'host: '/' diventa '.', lo spazio duro uno spazio, il tipo un suffisso
",xxx" (",llllllll-eeeeeeee" per i file con indirizzi di caricamento); la data
del file e' quella di RISC OS. I caratteri che Windows non accetta diventano '_'.
"""
import argparse
import os
import struct
import sys

HARD_SPACE = 0xA0
BAD_HOST_CHARS = '<>:"|?*\\'


# ---------------------------------------------------------------------------
# lettura dell'immagine
# ---------------------------------------------------------------------------

class OldMap:
    """formati L e D: indirizzi in unita' da 256 byte"""

    def __init__(self, img):
        self.img = img

    def read(self, addr, length):
        start = addr * 256
        return self.img[start:start + length]


class NewMap:
    """formato E: mappa a frammenti (una o piu' zone)"""

    def __init__(self, img, map_start=0):
        self.img = img
        dr = img[map_start + 4:map_start + 64]
        (self.log2secsize, _spt, _heads, _density, self.idlen, self.log2bpmb, _skew, _boot,
         _low, self.nzones, self.zone_spare, self.root) = struct.unpack_from("<BBBBBBBBBBHI", dr, 0)
        self.secsize = 1 << self.log2secsize
        self.zone_bits = (8 << self.log2secsize) - self.zone_spare
        self.zones = [img[map_start + z * self.secsize:map_start + (z + 1) * self.secsize]
                      for z in range(self.nzones)]
        self.disc_size = struct.unpack_from("<I", dr, 16)[0]

    def fragments(self, frag_id):
        """(indirizzo in byte, lunghezza in byte) dei pezzi del frammento, in ordine"""
        out = []
        dr_bits = 60 * 8
        for z, zone in enumerate(self.zones):
            bits = int.from_bytes(zone[4:], "little")       # dopo i 4 byte di intestazione
            pos = dr_bits if z == 0 else 0
            end = self.zone_bits
            while pos + self.idlen < end:
                fid = (bits >> pos) & ((1 << self.idlen) - 1)
                stop = pos + self.idlen
                while stop < end and not (bits >> stop) & 1:
                    stop += 1
                if fid == frag_id:
                    disc_bit = z * self.zone_bits + pos - dr_bits
                    out.append((disc_bit << self.log2bpmb, (stop + 1 - pos) << self.log2bpmb))
                pos = stop + 1
        return out

    def read(self, ind, length):
        frag, sector = ind >> 8, ind & 0xFF
        data = b"".join(self.img[a:a + n] for a, n in self.fragments(frag))
        off = (sector - 1) * self.secsize if sector else 0
        return data[off:off + length]


def detect(img):
    """(mappa, indirizzo della root, dimensione delle directory, voci massime)"""
    if img[0x401:0x405] == b"Hugo":
        return OldMap(img), 0x400 // 256, 0x800, 77
    if img[0x201:0x205] == b"Hugo":
        return OldMap(img), 0x200 // 256, 0x500, 47
    if len(img) > 64 and img[4] in (8, 9, 10) and img[8] >= 10:
        m = NewMap(img)
        return m, m.root, 0x800, 77
    raise ValueError("formato ADFS non riconosciuto (sono gestiti L, D ed E)")


def read_dir(disc, addr, size, max_entries):
    d = disc.read(addr, size)
    if d[1:5] not in (b"Hugo", b"Nick"):
        raise ValueError("directory rovinata a &%X" % addr)
    entries = []
    for i in range(max_entries):
        p = 5 + i * 26
        if d[p] == 0:
            break
        raw = d[p:p + 10]
        name = bytearray()
        for c in raw:
            if c < 0x20:
                break
            name.append(c)
        load, exec_, length = struct.unpack_from("<III", d, p + 10)
        ind = int.from_bytes(d[p + 22:p + 25], "little")
        attr = d[p + 25]
        entries.append((bytes(name), load, exec_, length, ind, attr))
    return entries


# ---------------------------------------------------------------------------
# scrittura sull'host
# ---------------------------------------------------------------------------

def host_name(name, load, exec_, is_dir):
    s = ""
    for c in name:
        ch = "." if c == ord("/") else " " if c == HARD_SPACE else bytes([c]).decode("latin-1")
        s += "_" if ch in BAD_HOST_CHARS else ch
    if is_dir:
        return s
    if (load >> 20) == 0xFFF:
        t = (load >> 8) & 0xFFF
        return s if t == 0xFFF else "%s,%03x" % (s, t)
    return "%s,%08x-%08x" % (s, load, exec_)


def set_date(path, load, exec_):
    if (load >> 20) != 0xFFF:
        return
    cs = ((load & 0xFF) << 32) | exec_
    t = cs / 100 - 2208988800
    if t > 0:
        try:
            os.utime(path, (t, t))
        except OSError:
            pass


def extract(disc, addr, size, max_entries, dest, listing, depth=0):
    count = 0
    for name, load, exec_, length, ind, attr in read_dir(disc, addr, size, max_entries):
        is_dir = bool(attr & 0x08)
        hn = host_name(name, load, exec_, is_dir)
        if listing:
            kind = "DIR " if is_dir else ("%03X " % ((load >> 8) & 0xFFF) if (load >> 20) == 0xFFF else "    ")
            print("%s%s %-12s %8d" % ("  " * depth, kind, name.decode("latin-1"), 0 if is_dir else length))
        path = os.path.join(dest, hn)
        if is_dir:
            if not listing:
                os.makedirs(path, exist_ok=True)
            count += extract(disc, ind, size, max_entries, path, listing, depth + 1)
        else:
            data = disc.read(ind, length)
            if len(data) != length:
                print("attenzione: %s letto %d byte su %d" % (hn, len(data), length), file=sys.stderr)
            if not listing:
                with open(path, "wb") as f:
                    f.write(data)
                set_date(path, load, exec_)
            count += 1
    return count


def extract_tree(node, dest, listing, depth=0):
    """albero di mkadfs.Node (da zip o Spark) -> cartella dell'host"""
    count = 0
    for c in node.children:
        hn = host_name(c.name, c.load or 0, c.exec, c.is_dir)
        if listing:
            kind = "DIR " if c.is_dir else ("%03X " % ((c.load >> 8) & 0xFFF) if (c.load >> 20) == 0xFFF else "    ")
            print("%s%s %-12s %8d" % ("  " * depth, kind, c.name.decode("latin-1"), 0 if c.is_dir else len(c.data)))
        path = os.path.join(dest, hn)
        if c.is_dir:
            if not listing:
                os.makedirs(path, exist_ok=True)
            count += extract_tree(c, path, listing, depth + 1)
        else:
            if not listing:
                with open(path, "wb") as f:
                    f.write(c.data)
                set_date(path, c.load, c.exec)
            count += 1
    return count


def expand_archives(dest, warnings):
    """Gli archivi dentro l'estrazione (",3fb" ArcFS, ",ddc" Spark, ",a91" e
    ".zip" zip) diventano cartelle con
    lo stesso nome, come li mostrerebbe SparkFS: cosi' i programmi partono
    senza SparkFS. Quelli che non si aprono restano come sono."""
    import mkadfs
    count = 0
    for dirpath, _dirs, files in os.walk(dest):
        for f in files:
            low = f.lower()
            if low[-4:] in (",3fb", ",ddc", ",a91"):
                cut = 4
            elif low.endswith(".zip"):              # zip senza tipo (es. CD fatti su PC)
                cut = 4
            else:
                continue
            path = os.path.join(dirpath, f)
            target = path[:-cut]
            if os.path.exists(target):
                continue
            w = []
            try:
                tree = mkadfs.from_archive(path, w)
            except Exception as e:              # archivio di un tipo che non sappiamo leggere
                warnings.append("archivio non aperto, lasciato com'e': %s (%s)" % (f, e))
                continue
            if len(tree.children) == 1 and tree.children[0].is_dir and \
                    tree.children[0].name.lower() == os.path.basename(target).encode("latin-1", "replace").lower():
                tree = tree.children[0]          # l'archivio contiene gia' la cartella omonima
            os.makedirs(target, exist_ok=True)
            count += extract_tree(tree, target, False)
            warnings.extend(w)
            os.remove(path)
    return count


def extract_any(src, dest, listing=False, expand=True):
    """immagine floppy, archivio o CD -> cartella; restituisce il numero di file"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkadfs
    import zipfile
    warnings = []
    low = src.lower()
    head = open(src, "rb").read(16)
    is_cd = low.endswith((".iso", ".bin", ".iso.zip", ".bincue.zip")) or head[:12] == b"\x00" + b"\xff" * 10 + b"\x00"
    if not is_cd and head[:2] == b"PK":
        names = [n.lower() for n in zipfile.ZipFile(src).namelist()]
        is_cd = any(n.endswith((".iso", ".bin")) for n in names) and len(names) <= 3
    if not listing:
        os.makedirs(dest, exist_ok=True)
    if is_cd:
        import cdextract
        n = cdextract.extract_cd(src, dest, listing)
    elif head[:8] == b"Archive\0" or head[:1] == b"\x1a" or head[:2] == b"PK":
        n = extract_tree(mkadfs.from_archive(src, warnings), dest or "", listing)
    else:
        img = open(src, "rb").read()
        disc, root, size, max_entries = detect(img)
        n = extract(disc, root, size, max_entries, dest or "", listing)
    while expand and not listing:                # anche gli archivi dentro gli archivi
        k = expand_archives(dest, warnings)
        if not k:
            break
        n += k
    for w in warnings:
        print("attenzione:", w, file=sys.stderr)
    return n


def main():
    ap = argparse.ArgumentParser(description="Estrae un'immagine floppy, un archivio o un CD RISC OS in una cartella (per HostFS)")
    ap.add_argument("immagine")
    ap.add_argument("cartella", nargs="?")
    ap.add_argument("--list", action="store_true", help="elenca soltanto")
    ap.add_argument("--keep-archives", action="store_true", help="non aprire gli archivi ArcFS/Spark contenuti")
    a = ap.parse_args()
    if not a.list and not a.cartella:
        ap.error("serve la cartella di destinazione (o --list)")
    n = extract_any(a.immagine, a.cartella, a.list, not a.keep_archives)
    if not a.list:
        print("%s: %d file in %s" % (os.path.basename(a.immagine), n, a.cartella))


if __name__ == "__main__":
    main()
