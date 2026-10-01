#!/bin/sh
#
# Crea il DMG per macOS con le due applicazioni (BBC BASIC e Archimedes):
# compilazione universale (Apple Silicon + Intel), firma con il Developer ID,
# DMG, notarizzazione e timbro. Da lanciare sul Mac, nella cartella del progetto:
#
#   sh tools/make_dmg.sh 1.0            (SKIP_NOTARIZE=1 per saltare Apple)
#
# Come per Runebrace: il portachiavi si sblocca con la password in
# ~/.rb-keychain (in SSH e' bloccato) e la notarizzazione usa il profilo
# 'runebrace' di notarytool (stesso account sviluppatore).
set -e

VERSION="${1:?serve la versione, per esempio 1.0}"
IDENTITY="${IDENTITY:-Developer ID Application: Renato Tarabella (VUCHTW4GLD)}"
PROFILE="${NOTARY_PROFILE:-runebrace}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-mac"
OUT="$ROOT/dist"
STAGE="$OUT/dmgroot"
DMG="$OUT/ArchieEmu-$VERSION-macOS.dmg"
export PATH="/opt/homebrew/bin:$PATH"

if [ -f "$HOME/.rb-keychain" ]; then
    security unlock-keychain -p "$(cat "$HOME/.rb-keychain")" >/dev/null 2>&1 \
        || { echo "sblocco del portachiavi fallito"; exit 1; }
fi

echo "== compilazione =="
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$BUILD" -j8 --target armwin archie >/dev/null

echo "== firma =="
# codice nativo: basta l'hardened runtime, nessun permesso speciale
for app in "ArchieEmu BASIC" "ArchieEmu Archimedes"; do
    codesign --force --timestamp --options runtime --sign "$IDENTITY" "$BUILD/$app.app"
    codesign --verify --strict --verbose=1 "$BUILD/$app.app" 2>&1 | tail -1
done

echo "== dmg =="
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$BUILD/ArchieEmu BASIC.app" "$BUILD/ArchieEmu Archimedes.app" "$STAGE/"
cp "$ROOT/tools/dist/README-macOS.txt" "$STAGE/README.txt"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "ArchieEmu $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
codesign --force --timestamp --sign "$IDENTITY" "$DMG"
rm -rf "$STAGE"

if [ -n "$SKIP_NOTARIZE" ]; then
    echo "notarizzazione saltata"
    ls -la "$DMG"
    exit 0
fi

echo "== notarizzazione =="
LOG="$OUT/notarize.log"
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait 2>&1 | tee "$LOG" | tail -3
grep -q "status: Accepted" "$LOG" \
    || { echo "NON accettata: xcrun notarytool log <id> --keychain-profile $PROFILE"; exit 1; }

echo "== timbro =="
xcrun stapler staple "$DMG"
spctl -a -t open --context context:primary-signature -v "$DMG" 2>&1 | tail -2
ls -la "$DMG"
