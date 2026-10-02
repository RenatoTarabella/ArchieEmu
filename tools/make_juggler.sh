#!/bin/sh
# Costruisce disc/Juggler,ffb dal listato demos/juggler.bas (TEXTLOAD + SAVE
# nella macchina BBC BASIC). Uso: tools/make_juggler.sh [cartella build]
set -e
cd "$(dirname "$0")/.."
B=${1:-build/Release}
cp demos/juggler.bas disc/jugtxt
printf 'TEXTLOAD "jugtxt"\nSAVE "Juggler"\n' | "$B/armbasic" --disc disc >/dev/null
rm disc/jugtxt
# le 24 scene dell'animazione (ricostruzione procedurale)
python3 tools/juggler_anim.py disc/Scenes/Anim 2>/dev/null || python tools/juggler_anim.py disc/Scenes/Anim
