#!/bin/sh
# Crea i due pacchetti portable per Linux (macchina BASIC e Archimedes) in dist/
#
#   sh tools/make_linux.sh 1.3
#
# Serve SDL2 per compilare: gli header di sistema (libsdl2-dev) oppure, senza
# root, CMAKE_ARGS con -DSDL2_INCLUDE_DIR=... -DSDL2_LIBRARY=... (vedi CLAUDE.md).
# Gli eseguibili usano la SDL2 del sistema (libsdl2-2.0-0) e glibc >= 2.34.
set -e
VERSION="${1:?serve la versione, per esempio 1.3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CMAKE="$(command -v cmake || echo "$HOME/.local/bin/cmake")"
BUILD="$ROOT/build-linux"
OUT="$ROOT/dist"
STAGE="$OUT/stage-linux"

"$CMAKE" -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release $CMAKE_ARGS >/dev/null
"$CMAKE" --build "$BUILD" -j"$(nproc)" --target armwin archie
strip "$BUILD/armwin" "$BUILD/archie"

rm -rf "$STAGE"
mkdir -p "$STAGE"

# macchina BBC BASIC: non serve nessuna ROM
B="$STAGE/ArchieEmu-BASIC"
mkdir -p "$B/third_party/riscos" "$B/disc"
cp "$BUILD/armwin" "$ROOT/LICENSE" "$B/"
cp "$ROOT/tools/dist/README-Linux.txt" "$B/README.txt"
cp "$ROOT/third_party/riscos/BASIC" "$ROOT/third_party/riscos/LICENSE" "$ROOT/third_party/riscos/README.md" \
   "$B/third_party/riscos/"
# solo i file del repository (se c'e' git), non quelli lasciati in locale
if git -C "$ROOT" rev-parse >/dev/null 2>&1; then
    (cd "$ROOT" && git ls-files disc | while read -r f; do mkdir -p "$B/$(dirname "$f")"; cp "$f" "$B/$f"; done)
else
    cp -R "$ROOT/disc/." "$B/disc/"
fi

# Archimedes e Risc PC: ROM e dischetti li mette l'utente
A="$STAGE/ArchieEmu-Archimedes"
mkdir -p "$A/roms/1. Major" "$A/ADF" "$A/HostFS"
cp "$BUILD/archie" "$ROOT/LICENSE" "$A/"
cp "$ROOT/tools/dist/README-Linux.txt" "$A/README.txt"
echo "Put your RISC OS ROM images here, named by version (not included: copyrighted): ROM311 for RISC OS 3.11 (Archimedes); ROM350, ROM360, ROM370 or ROM371 for the Risc PC." \
    > "$A/roms/1. Major/PUT_ROMS_HERE.txt"
echo "Put your .adf floppy images here (Disc > Insert floppy in the emulator)." > "$A/ADF/PUT_ADF_IMAGES_HERE.txt"
echo "This folder is the HostFS disc of RISC OS: whatever you put here appears in the emulator, and RISC OS can save here." \
    > "$A/HostFS/ReadMe"
# !Boot e l'MDF del Risc PC (il Display Manager della 3.5-3.7 ne ha bisogno)
cp -R "$ROOT/tools/dist/HostFS/." "$A/HostFS/"

for p in BASIC Archimedes; do
    T="$OUT/ArchieEmu-$p-v$VERSION-linux-x64.tar.gz"
    tar -C "$STAGE" -czf "$T" "ArchieEmu-$p"
    ls -l "$T"
done
