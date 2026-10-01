"""
mkadfs.py - Crea un'immagine floppy ADFS formato D (800 KB) da una cartella,
da uno zip RISC OS o da un archivio Spark/Arc, conservando nomi, tipi di file
e date di RISC OS.

    python tools/mkadfs.py sorgente immagine.adf [--name NomeDisco] [--root sotto/cartella]

Formato D (verificato sui dischi originali): mappa vecchia nei primi 512 byte
(inizi e lunghezze delle zone libere in unita' da 256 byte, con somma di
controllo), root "$" a &400 lunga &800, directory "Hugo" da 77 voci di 26 byte
con byte di controllo calcolato come in FileCore (s.FileCore25). Ogni oggetto
deve cominciare su un settore da 1 KB: ADFS rifiuta gli altri ("Bad
parameters", che con la cache dei dischi si perde e blocca FileCore).

Zip: il tipo arriva dal campo extra RISC OS ("AC" + "ARC0": load, exec, attr);
senza, dal suffisso ",xxx" o dal nome. Spark: metodo 2 (memorizzato) e 127
("compress", LZW di Unix), con controllo del CRC.
"""
import argparse
import os
import random
import shutil
import subprocess
import tempfile
import struct
import sys
import time
import zipfile

DISC_SIZE = 819200
UNIT = 256                    # unita' della mappa
SECTOR_UNITS = 4              # i file si allocano a settori interi da 1 KB
DIR_SIZE = 0x800
MAX_ENTRIES = 77


class Node:
    def __init__(self, name, is_dir=False, data=b"", load=None, exec_=0, attr=3):
        self.name = name              # bytes Latin-1, senza percorso
        self.is_dir = is_dir
        self.children = []
        self.data = data
        self.load = load
        self.exec = exec_
        self.attr = attr
        self.addr = 0                 # in unita' da 256 byte

    def child_dir(self, name):
        for c in self.children:
            if c.is_dir and c.name.lower() == name.lower():
                return c
        d = Node(name, True)
        self.children.append(d)
        return d


# ---------------------------------------------------------------------------
# tipi e date
# ---------------------------------------------------------------------------

def riscos_time(t=None):
    """centesimi di secondo dal 1900 (40 bit)"""
    t = time.time() if t is None else t
    return int((t + 2208988800) * 100) & 0xFFFFFFFFFF


def load_exec_for(ftype, t=None):
    cs = riscos_time(t)
    return 0xFFF00000 | (ftype & 0xFFF) << 8 | (cs >> 32), cs & 0xFFFFFFFF


def guess_type(name, data):
    n = name.lower()
    if n in (b"!run", b"!boot") or n.endswith(b"obey"):
        return 0xFEB
    if b"sprites" in n:
        return 0xFF9
    if n == b"templates":
        return 0xFEC
    if len(data) >= 0x14 and struct.unpack_from("<I", data, 0x10)[0] == 0xEF000011:
        return 0xFF8                  # immagine AIF: SWI OS_Exit a +&10
    if n == b"!runimage":
        return 0xFF8
    return 0xFFF


# ---------------------------------------------------------------------------
# sorgenti
# ---------------------------------------------------------------------------

def name_from_host(s):
    """nome dell'host -> RISC OS: '.' e '/' si scambiano, tipo dal suffisso ,xxx"""
    ftype = None
    if len(s) > 4 and s[-4] == ",":
        try:
            ftype = int(s[-3:], 16)
            s = s[:-4]
        except ValueError:
            pass
    s = s.replace(".", "\x00").replace("/", ".").replace("\x00", "/")
    return s.encode("latin-1", "replace"), ftype


def from_directory(path):
    root = Node(b"$", True)

    def add(dirnode, p):
        for entry in sorted(os.listdir(p)):
            full = os.path.join(p, entry)
            name, ftype = name_from_host(entry)
            if os.path.isdir(full):
                add(dirnode.child_dir(name), full)
            else:
                data = open(full, "rb").read()
                if ftype is None:
                    ftype = guess_type(name, data)
                load, exe = load_exec_for(ftype, os.path.getmtime(full))
                dirnode.children.append(Node(name, data=data, load=load, exec_=exe))
    add(root, path)
    return root


def zip_riscos_info(extra):
    """campo extra RISC OS (SparkFS): id 0x4341 'AC', firma ARC0, load, exec, attr"""
    p = 0
    while p + 4 <= len(extra):
        hid, size = struct.unpack_from("<HH", extra, p)
        body = extra[p + 4:p + 4 + size]
        if hid == 0x4341 and body[:4] == b"ARC0" and len(body) >= 16:
            return struct.unpack_from("<III", body, 4)
        p += 4 + size
    return None


SEVEN_ZIP = next((p for p in (r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe")
                  if os.path.exists(p)), None)


class _LenientZipInfo(zipfile.ZipInfo):
    """alcuni zip di SparkFS dichiarano il campo extra RISC OS piu' lungo di
    quello che c'e': Python rifiuterebbe tutto l'archivio ("Corrupt extra
    field"). Il campo lo legge comunque zip_riscos_info, con i byte presenti."""
    def _decodeExtra(self, *args):
        try:
            super()._decodeExtra(*args)
        except zipfile.BadZipFile:
            pass


def from_zip(path, warnings):
    root = Node(b"$", True)
    saved = zipfile.ZipInfo
    zipfile.ZipInfo = _LenientZipInfo
    try:
        z = zipfile.ZipFile(path)
    finally:
        zipfile.ZipInfo = saved
    # Implode (metodo 6) e altri metodi antichi: Python non li legge, 7-Zip si'.
    # Dallo zip si prendono comunque i tipi RISC OS dei campi extra.
    extracted = None
    if any(i.compress_type not in (0, 8) for i in z.infolist()):
        if not SEVEN_ZIP:
            raise ValueError("serve 7-Zip per i metodi di compressione di questo zip")
        extracted = tempfile.mkdtemp(prefix="mkadfs_")
        subprocess.run([SEVEN_ZIP, "x", "-y", "-o" + extracted, path], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for info in z.infolist():
        raw = info.filename.encode("cp437") if not (info.flag_bits & 0x800) else info.filename.encode("utf-8")
        parts = [x for x in raw.split(b"/") if x]
        if not parts:
            continue
        # i nomi RISC OS negli zip hanno '.' al posto di '/': si rimettono
        parts = [x.replace(b".", b"/") for x in parts]
        node = root
        for d in parts[:-1]:
            node = node.child_dir(d)
        if info.is_dir():
            node.child_dir(parts[-1])
            continue
        if extracted:
            host = os.path.join(extracted, *info.filename.split("/"))
            if not os.path.exists(host):
                warnings.append("non estratto da 7-Zip, saltato: " + info.filename)
                continue
            data = open(host, "rb").read()
        else:
            data = z.read(info)
        ri = zip_riscos_info(info.extra)
        name = parts[-1]
        if ri:
            load, exe, attr = ri
            node.children.append(Node(name, data=data, load=load, exec_=exe, attr=(attr & 0xFF) or 3))
        else:
            host, ftype = name_from_host(name.decode("latin-1"))
            if ftype is None:
                ftype = guess_type(host, data)
            load, exe = load_exec_for(ftype)
            node.children.append(Node(host, data=data, load=load, exec_=exe))
    if extracted:
        shutil.rmtree(extracted, ignore_errors=True)
    return root


def lzw_uncompress(data, size):
    """'compress' di Unix come negli archivi Arc/Spark: il primo byte e' il
    numero massimo di bit, poi codici da 9 bit in su, CLEAR = 256, e il
    riallineamento a gruppi di 8 codici quando cambia la larghezza."""
    maxbits = data[0] & 0x1F
    src = data[1:]
    out = bytearray()
    bitpos = 0
    nbits = 9
    table_prefix = list(range(256)) + [0] * ((1 << maxbits) - 256)
    table_suffix = list(range(256)) + [0] * ((1 << maxbits) - 256)
    free = 257
    old = -1
    finchar = 0
    group_start = 0
    total_bits = len(src) * 8
    while bitpos + nbits <= total_bits and len(out) < size:
        if free > (1 << nbits) - 1 and nbits < maxbits:
            # nuova larghezza: si salta al confine del gruppo di 8 codici
            group_bits = nbits * 8
            used = bitpos - group_start
            if used % group_bits:
                bitpos += group_bits - used % group_bits
            nbits += 1
            group_start = bitpos
            continue
        byte = bitpos >> 3
        chunk = int.from_bytes(src[byte:byte + 4].ljust(4, b"\0"), "little")
        code = (chunk >> (bitpos & 7)) & ((1 << nbits) - 1)
        bitpos += nbits
        if code == 256:
            group_bits = nbits * 8
            used = bitpos - group_start
            if used % group_bits:
                bitpos += group_bits - used % group_bits
            nbits = 9
            free = 256                # il prossimo codice nuovo sara' 257
            old = -1
            group_start = bitpos
            continue
        if old == -1:
            out.append(code)
            old = finchar = code
            free = max(free, 257)
            continue
        incode = code
        stack = bytearray()
        if code >= free:              # caso KwKwK
            stack.append(finchar)
            code = old
        while code >= 256:
            stack.append(table_suffix[code])
            code = table_prefix[code]
        finchar = code
        stack.append(finchar)
        out.extend(reversed(stack))
        if free < (1 << maxbits):
            table_prefix[free] = old
            table_suffix[free] = finchar
            free += 1
        old = incode
    return bytes(out[:size])


def arc_crc(data):
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def from_spark(path, warnings):
    d = open(path, "rb").read()
    root = Node(b"$", True)

    def walk(node, p, end):
        while p < end:
            if d[p] != 0x1A:
                raise ValueError("archivio Spark rovinato a &%X" % p)
            m = d[p + 1]
            method = m & 0x7F
            if method == 0:
                return p + 2
            name = d[p + 2:p + 15].split(b"\0")[0]
            csize = struct.unpack_from("<I", d, p + 15)[0]
            crc = struct.unpack_from("<H", d, p + 23)[0]
            hdr = 29 if method > 1 else 25
            size = struct.unpack_from("<I", d, p + 25)[0] if method > 1 else csize
            load, exe, attr = struct.unpack_from("<III", d, p + hdr) if m & 0x80 else (0xFFFFFD00, 0, 3)
            if m & 0x80:
                hdr += 12
            body = d[p + hdr:p + hdr + csize]
            if (load >> 8) & 0xFFFFF == 0xFFDDC:          # cartella: archivio annidato
                walk(node.child_dir(name), p + hdr, p + hdr + csize)
            else:
                if method == 2:
                    data = body
                elif method == 127:
                    data = lzw_uncompress(body, size)
                else:
                    raise ValueError("metodo Spark %d non gestito (%s)" % (method, name))
                if arc_crc(data) != crc:
                    # archivio danneggiato: meglio un disco con un file in meno
                    warnings.append("CRC sbagliato (archivio danneggiato), saltato: %s" % name.decode("latin-1"))
                    p += hdr + csize
                    continue
                node.children.append(Node(name, data=data, load=load, exec_=exe, attr=attr & 0xFF or 3))
            p += hdr + csize
        return p
    walk(root, 0, len(d))
    return root


# ---------------------------------------------------------------------------
# immagine ADFS D
# ---------------------------------------------------------------------------

def ror(x, n):
    return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF


def map_checksum(b):
    s = c = 0
    for i in range(254, -1, -1):
        t = s + b[i] + c
        c, s = t >> 8, t & 255
    return s


def dir_check(d, entries_end):
    acc = 0
    p = 0
    while p < entries_end & ~3:
        acc = struct.unpack_from("<I", d, p)[0] ^ ror(acc, 13)
        p += 4
    while p < entries_end:
        acc = d[p] ^ ror(acc, 13)
        p += 1
    p = DIR_SIZE - 40
    while p < DIR_SIZE - 4:
        acc = struct.unpack_from("<I", d, p)[0] ^ ror(acc, 13)
        p += 4
    acc ^= acc >> 16
    acc ^= acc >> 8
    return acc & 0xFF


def fit_name(name, warnings, where):
    if len(name) > 10:
        warnings.append("nome troncato a 10 caratteri: %s/%s" % (where, name.decode("latin-1")))
        name = name[:10]
    return name


def pad_name(name, size):
    n = name[:size]
    if len(n) < size:
        n += b"\r"
    return n.ljust(size, b"\0")


def build(root, disc_name, warnings):
    units = lambda n: (n + UNIT - 1) // UNIT
    # allocazione: root a &400, poi directory e file uno dopo l'altro
    root.addr = 0x400 // UNIT
    nxt = [root.addr + DIR_SIZE // UNIT]

    def alloc(node, path):
        node.children.sort(key=lambda c: c.name.lower())
        if len(node.children) > MAX_ENTRIES:
            raise ValueError("%s: %d voci, il formato D ne ammette %d" % (path, len(node.children), MAX_ENTRIES))
        for c in node.children:
            c.name = fit_name(c.name, warnings, path)
            if c.is_dir:
                c.addr = nxt[0]
                nxt[0] += DIR_SIZE // UNIT
                alloc(c, path + "/" + c.name.decode("latin-1"))
            else:
                c.addr = nxt[0]
                sectors = max(1, -(-units(len(c.data)) // SECTOR_UNITS))
                nxt[0] += sectors * SECTOR_UNITS
    alloc(root, "$")
    total = DISC_SIZE // UNIT
    if nxt[0] > total:
        raise ValueError("non entra in 800 KB: servono %d KB" % (nxt[0] * UNIT // 1024))

    img = bytearray(DISC_SIZE)

    def write_dir(node, parent_addr):
        d = bytearray(DIR_SIZE)
        d[1:5] = b"Hugo"
        p = 5
        for c in node.children:
            load = c.load if c.load is not None else 0xFFFFFD00
            length = DIR_SIZE if c.is_dir else len(c.data)
            attr = 0x08 if c.is_dir else (c.attr & 0x77 or 3)
            d[p:p + 10] = pad_name(c.name, 10)
            struct.pack_into("<III", d, p + 10, load, c.exec, length)
            d[p + 22:p + 25] = c.addr.to_bytes(3, "little")
            d[p + 25] = attr
            p += 26
        end_entries = p                         # qui c'e' lo 0 di fine elenco
        tail = DIR_SIZE - 40
        d[tail + 2:tail + 5] = parent_addr.to_bytes(3, "little")
        label = node.name if node is not root else b"$"
        d[tail + 5:tail + 24] = (label + b"\r" if len(label) < 19 and node is not root else label).ljust(19, b"\0")[:19]
        d[tail + 24:tail + 34] = (pad_name(label, 10) if node is not root else b"$".ljust(10, b"\0"))
        d[DIR_SIZE - 5:DIR_SIZE - 1] = b"Hugo"
        d[DIR_SIZE - 1] = dir_check(d, end_entries)
        img[node.addr * UNIT:node.addr * UNIT + DIR_SIZE] = d
        for c in node.children:
            if c.is_dir:
                write_dir(c, node.addr)
            else:
                img[c.addr * UNIT:c.addr * UNIT + len(c.data)] = c.data

    write_dir(root, root.addr)

    # mappa: una sola zona libera dalla fine dei dati alla fine del disco
    m = bytearray(512)
    free_start, free_len = nxt[0], total - nxt[0]
    entries = 0
    if free_len > 0:
        m[0:3] = free_start.to_bytes(3, "little")
        m[256:259] = free_len.to_bytes(3, "little")
        entries = 1
    m[0xFC:0xFF] = total.to_bytes(3, "little")
    name = disc_name.encode("latin-1", "replace")[:10].ljust(10, b"\0")
    m[0xF7:0xFC] = name[0::2]                  # caratteri pari
    m[0x1F6:0x1FB] = name[1::2]                # caratteri dispari
    m[0x1FB:0x1FD] = random.randrange(1, 0xFFFF).to_bytes(2, "little")
    m[0x1FD] = 0                               # nessuna opzione di boot
    m[0x1FE] = entries * 3
    m[0xFF] = map_checksum(m[0:256])
    m[0x1FF] = map_checksum(m[256:512])
    img[0:512] = m
    return bytes(img), warnings, nxt[0] * UNIT


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sorgente")
    ap.add_argument("immagine")
    ap.add_argument("--name", default=None, help="nome del disco (max 10 caratteri)")
    ap.add_argument("--root", default=None, help="usa come root questa sottocartella della sorgente")
    ap.add_argument("--only", default=None, help="solo questi elementi della root, separati da virgole")
    ap.add_argument("--sizes", action="store_true", help="elenca gli elementi della root con la dimensione")
    a = ap.parse_args()

    src = a.sorgente
    warnings = []
    if os.path.isdir(src):
        root = from_directory(src)
    else:
        head = open(src, "rb").read(4)
        root = from_spark(src, warnings) if head[:1] == b"\x1a" else from_zip(src, warnings)
    if a.root:
        for part in a.root.encode("latin-1").split(b"/"):
            root = next(c for c in root.children if c.is_dir and c.name.lower() == part.lower())
        root.name = b"$"
    elif len(root.children) == 1 and root.children[0].is_dir and not root.children[0].name.startswith(b"!"):
        root = root.children[0]                # cartella contenitore (es. "elite/")
        root.name = b"$"
    if a.sizes:
        def kb(n):
            return (sum(kb(c) for c in n.children) + 2) if n.is_dir else (len(n.data) + 1023) // 1024
        for c in root.children:
            print("%-12s %5d KB" % (c.name.decode("latin-1"), kb(c)))
        return
    if a.only:
        keep = [x.strip().encode("latin-1").lower() for x in a.only.split(",")]
        root.children = [c for c in root.children if c.name.lower() in keep]
    disc_name = a.name or os.path.splitext(os.path.basename(src))[0].split("(")[0].strip().replace(" ", "")[:10]
    img, warnings, used = build(root, disc_name, warnings)
    open(a.immagine, "wb").write(img)
    for w in warnings:
        print("attenzione:", w)
    print("%s: %d KB usati su 800, disco '%s'" % (a.immagine, used // 1024, disc_name))


if __name__ == "__main__":
    main()
