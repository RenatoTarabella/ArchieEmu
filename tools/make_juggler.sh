#!/bin/sh
# Costruisce disc/Juggler,ffb dal listato demos/juggler.bas (TEXTLOAD + SAVE
# nella macchina BBC BASIC). Uso: tools/make_juggler.sh [cartella build]
set -e
cd "$(dirname "$0")/.."
B=${1:-build/Release}
cp demos/juggler.bas disc/jugtxt
printf 'TEXTLOAD "jugtxt"\nSAVE "Juggler"\n' | "$B/armbasic" --disc disc >/dev/null
rm disc/jugtxt
