"""
cdextract.py - Estrae un CD-ROM RISC OS (ISO 9660) in una cartella dell'host,
con i nomi e i tipi di RISC OS nella convenzione di HostFS/RPCEmu.

    python tools/cdextract.py immagine.iso|immagine.iso.zip|immagine.bincue.zip|immagine.bin cartella [--list]

Lo usa anche adfextract.py, che gli passa le immagini di CD.

I CD fatti per RISC OS hanno, nell'area "system use" di ogni voce di
directory, l'estensione Acorn: "ARCHIMEDES", poi load, exec e attributi
(4 byte ciascuno). Il '!' non e' ammesso nei nomi ISO: si scrive '_' e il
bit 8 degli attributi dice di rimetterlo. Senza estensione il tipo viene dal
suffisso ",xxx" del nome, se c'e', o e' Text. Nei nomi ISO si tolgono ";1" e
il punto finale; il '.' dell'estensione diventa '/' come in CDFS.

Immagini .bin (da .cue): settori da 2352 byte, modo 1 (dati a +16) o modo 2
forma 1 (dati a +24). Gli zip si scompattano in una cartella temporanea.
"""
import argparse
import os
import shutil
import struct
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from adfextract import host_name, set_date      # noqa: E402

SECTOR = 2048


class Image:
    """settori logici da 2048 byte di un .iso o di un .bin"""

    def __init__(self, path):
        self.f = open(path, "rb")
        size = os.path.getsize(path)
        head = self.f.read(16)
        self.raw = 0
        self.offset = 0
        if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00":       # sincronismo: settori grezzi
            self.raw = 2352
            mode = head[15]
            self.offset = 16 if mode == 1 else 24
        elif size % 2352 == 0 and size % 2048 != 0:
            self.raw, self.offset = 2352, 16

    def read(self, lba, length):
        if not self.raw:
            self.f.seek(lba * SECTOR)
            return self.f.read(length)
        out = bytearray()
        while len(out) < length:
            self.f.seek(lba * self.raw + self.offset)
            out += self.f.read(SECTOR)
            lba += 1
        return bytes(out[:length])


def acorn_info(rec, name_len):
    """(load, exec, attr) dall'estensione ARCHIMEDES, o None"""
    su = 33 + name_len + (0 if name_len % 2 else 1)
    area = rec[su:]
    i = area.find(b"ARCHIMEDES")
    if i < 0 or len(area) < i + 22:
        return None
    return struct.unpack_from("<III", area, i + 10)


def ro_name(iso_name, info):
    """nome ISO -> nome RISC OS (bytes Latin-1)"""
    n = iso_name.split(b";")[0]
    if n.endswith(b".") and not info:
        n = n[:-1]
    if info and info[2] & 0x100 and n.startswith(b"_"):
        n = b"!" + n[1:]
    return n.replace(b".", b"/")


def walk(img, lba, length, dest, listing, depth=0):
    count = 0
    data = img.read(lba, length)
    p = 0
    while p < len(data):
        rec_len = data[p]
        if rec_len == 0:                                # resto del settore vuoto
            p = (p // SECTOR + 1) * SECTOR
            continue
        rec = data[p:p + rec_len]
        p += rec_len
        name_len = rec[32]
        iso = rec[33:33 + name_len]
        if iso in (b"\x00", b"\x01"):                   # "." e ".."
            continue
        ext_lba, size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        is_dir = bool(rec[25] & 2)
        info = acorn_info(rec, name_len)
        name = ro_name(iso, info)
        if info:
            load, exec_ = info[0], info[1]
        else:
            load, exec_ = 0xFFFFFF00, 0                 # Text
            text = name.decode("latin-1")
            if len(text) > 4 and text[-4] == "," and all(c in "0123456789abcdefABCDEF" for c in text[-3:]):
                load = 0xFFF00000 | int(text[-3:], 16) << 8
                name = name[:-4]
        hn = host_name(name, load, exec_, is_dir)
        if listing:
            kind = "DIR " if is_dir else ("%03X " % ((load >> 8) & 0xFFF) if (load >> 20) == 0xFFF else "    ")
            print("%s%s %-20s %9d" % ("  " * depth, kind, name.decode("latin-1"), 0 if is_dir else size))
        path = os.path.join(dest, hn)
        if is_dir:
            if not listing:
                os.makedirs(path, exist_ok=True)
            count += walk(img, ext_lba, size, path, listing, depth + 1)
        else:
            if not listing:
                with open(path, "wb") as f:
                    left, at = size, ext_lba
                    while left:
                        chunk = min(left, 64 * SECTOR)
                        f.write(img.read(at, chunk))
                        left -= chunk
                        at += chunk // SECTOR
                set_date(path, load, exec_)
            count += 1
    return count


def open_image(path, tmp):
    """percorso dell'immagine da leggere (scompattando uno zip se serve)"""
    if not zipfile.is_zipfile(path):
        return path
    z = zipfile.ZipFile(path)
    members = [i for i in z.infolist() if not i.is_dir()]
    pick = [i for i in members if i.filename.lower().endswith((".iso", ".bin", ".img"))] or \
        sorted(members, key=lambda i: -i.file_size)[:1]
    if not pick:
        raise ValueError("nessuna immagine di CD nello zip")
    target = os.path.join(tmp, os.path.basename(pick[0].filename))
    with z.open(pick[0]) as src, open(target, "wb") as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
    return target


def extract_cd(path, dest, listing=False):
    tmp = tempfile.mkdtemp(prefix="cdextract_")
    try:
        img = Image(open_image(path, tmp))
        pvd = img.read(16, SECTOR)
        if pvd[1:6] != b"CD001":
            raise ValueError("non e' un CD ISO 9660")
        root = pvd[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        if not listing:
            os.makedirs(dest, exist_ok=True)
        n = walk(img, lba, size, dest or "", listing)
        img.f.close()
        return n
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="Estrae un CD-ROM RISC OS in una cartella (per HostFS)")
    ap.add_argument("immagine")
    ap.add_argument("cartella", nargs="?")
    ap.add_argument("--list", action="store_true", help="elenca soltanto")
    a = ap.parse_args()
    if not a.list and not a.cartella:
        ap.error("serve la cartella di destinazione (o --list)")
    n = extract_cd(a.immagine, a.cartella, a.list)
    if not a.list:
        print("%s: %d file in %s" % (os.path.basename(a.immagine), n, a.cartella))


if __name__ == "__main__":
    main()
