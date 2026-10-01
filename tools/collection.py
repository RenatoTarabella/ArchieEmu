"""
collection.py - Prepara in una cartella HostFS una raccolta di giochi pronti:
per ogni titolo le immagini dei dischetti (.adf, o .hfe se non c'e' un .adf),
una per disco, con nomi puliti; i titoli che esistono solo come archivio (zip,
Spark, ArcFS) vengono estratti. In RISC OS un doppio clic sul dischetto lo
inserisce nell'unita' 0 e apre la finestra (tipo &FCE, *HostFS_Insert).

    python tools/collection.py "X:/Archimedes archive" HostFS/Classics [titoli.txt]

L'archivio e' quello di arcarc.nl (Games/<lettera>/<titolo>/...). Senza
titoli.txt si usa la lista qui sotto; nel file, un titolo per riga
(il nome della cartella nell'archivio, o un suo inizio).
"""
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

CLASSICS = """
Zarch / Lander (Zarch Demo)
Play it Again Sam 1 (Conqueror, Hostages, No Excuses, Rotor)
Play it Again Sam 2 (Zarch, Master Break, Repton 2, Pinball)
Play it Again Sam 3 (Superior Golf, Zelanites, Letrouve, Top Banana)
Elite
Conqueror
Chocks Away
Interdictor
Interdictor 2
Lemmings
Lemmings 2 - The Tribes
Cannon Fodder
Gods
James Pond
James Pond 2 - Codename Robocod
Chuck Rock
Fire & Ice
Nebulus
Mad Professor Mariarti
Nevryon
Apocalypse
Black Angel
Cops
Chaos Engine, The
Flashback
Gribblys Day Out
Lotus Turbo Challenge 2
E-Type
Arcade Soccer
Holed Out 2
ArcPinball
Battle Chess
Hero Quest
Heimdall
Populous
Pacmania
Speedball 2
Starfighter 3000
Global Effect
Dune II
Days of Steam
Great Gianna Sisters, The
Last Ninja, The
Oddball
Iron Lord
Asylum
Ego - Repton 4
Bubble Impact (FR)
Guild of Thieves
Corruption
Fish - Topologika
Jinxter
Magnetic Scrolls Collection (Fish!, Corruption, Guild of Thieves)
Twin World
Saloon Cars
Powerband
Drifter
Fervour
Blood Sport
Hamsters
Ixion
Cycloids
Darkwood
Big Bang
Revolver
"""

DISC_RE = re.compile(r"(?:dis[ck]k?|disk)\s*(\d+)\s*(?:of\s*\d+)?", re.I)


def disc_number(name):
    m = DISC_RE.search(name)
    return int(m.group(1)) if m else 1


def badness(name):
    """piu' basso = immagine migliore"""
    n = name.lower()
    score = 0
    score += 10 * len(re.findall(r"\[[^\]]*\]", n))            # [a], [b], [cr], [h]...
    for w in ("patch", "parched", "modified", "demo", "crack", "hack", "trained", "cheat", "alt"):
        if w in n:
            score += 20
    if n.endswith(".hfe"):
        score += 5                                             # .adf se c'e' (scrivibile, piu' piccolo)
    return score, len(n)


def clean(s):
    s = re.sub(r"\s*\[[^\]]*\]", "", s)
    s = re.sub(r"[<>:\"|?*\\/]", "_", s)
    return s.strip()


def find_title(games, title):
    want = title.lower()
    best = None
    for letter in sorted(os.listdir(games)):
        ld = os.path.join(games, letter)
        if not os.path.isdir(ld):
            continue
        for t in os.listdir(ld):
            tl = t.lower()
            if tl == want:
                return os.path.join(ld, t)
            if tl.startswith(want) and best is None:
                best = os.path.join(ld, t)
    return best


def pick_images(folder):
    imgs = []
    for root, _dirs, files in os.walk(folder):
        for f in files:
            if f.lower().endswith((".adf", ".hfe", ".adl")):
                imgs.append(os.path.join(root, f))
    # raggruppati per "edizione" (nome senza disco e varianti) e per numero di disco
    groups = {}
    for p in imgs:
        base = os.path.basename(p)
        key = re.sub(r"\s*\((?:dis[ck]k?|disk)[^)]*\)", "", os.path.splitext(base)[0], flags=re.I)
        key = re.sub(r"\s*\[[^\]]*\]", "", key).strip().lower()
        groups.setdefault(key, {}).setdefault(disc_number(base), []).append(p)
    if not groups:
        return []
    # l'edizione con piu' dischi completi e le immagini migliori
    def group_rank(item):
        _key, discs = item
        return (-len(discs), sum(min(badness(os.path.basename(x)) for x in v)[0] for v in discs.values()))
    _key, discs = sorted(groups.items(), key=group_rank)[0]
    return [min(v, key=lambda x: badness(os.path.basename(x))) for _n, v in sorted(discs.items())]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    games = os.path.join(sys.argv[1], "Games")
    dest = sys.argv[2]
    titles = [t.strip() for t in (open(sys.argv[3], encoding="utf-8").read() if len(sys.argv) > 3 else CLASSICS).splitlines()]
    titles = [t for t in titles if t]
    os.makedirs(dest, exist_ok=True)
    for entry in titles:
        options = [x.strip() for x in entry.split(" / ")]
        folder = next((f for f in (find_title(games, o) for o in options) if f), None)
        if not folder:
            print("non trovato:", entry)
            continue
        imgs = pick_images(folder)
        name = clean(os.path.basename(folder))
        if not imgs:
            # niente dischetti: l'archivio zip/Spark/ArcFS, estratto (versione da disco fisso)
            arcs = sorted((os.path.join(r, f) for r, _d, fs in os.walk(folder) for f in fs
                           if f.lower().endswith((".zip", ".arc", ".spk")) and not f.lower().endswith((".iso.zip", ".bincue.zip"))),
                          key=lambda x: badness(os.path.basename(x)))
            if not arcs:
                print("nessun dischetto ne' archivio (solo CD):", os.path.basename(folder))
                continue
            import adfextract
            out = os.path.join(dest, name)
            try:
                n = adfextract.extract_any(arcs[0], out)
            except Exception as e:
                print("archivio non estratto: %s (%s)" % (os.path.basename(arcs[0]), e))
                shutil.rmtree(out, ignore_errors=True)
                continue
            print("%-60s estratto, %d file" % (name, n))
            continue
        out = os.path.join(dest, name)
        os.makedirs(out, exist_ok=True)
        for i, src in enumerate(imgs, 1):
            ext = os.path.splitext(src)[1].lower()
            target = os.path.join(out, (name if len(imgs) == 1 else "%s - Disc %d" % (name, i)) + ext)
            shutil.copyfile(src, target)
        print("%-60s %d %s" % (name, len(imgs), ", ".join(os.path.splitext(x)[1] for x in imgs)))


if __name__ == "__main__":
    main()
