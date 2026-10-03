#!/bin/sh
# Run a Multi VirAnalyzer program with the bridge enabled.
#
#   scripts/run.sh [PROGRAM.exe]      (default: ProfessionalVersion.exe)
#
# Environment:
#   WINEPREFIX  Wine prefix the vendor software is installed in (default ~/.wine)
#   APPDIR      program directory (default: its standard path inside WINEPREFIX)
set -eu

WINEPREFIX="${WINEPREFIX:-$HOME/.wine}"
APPDIR="${APPDIR:-$WINEPREFIX/drive_c/Program Files/Vimu Electronic/Multi VirAnalyzer}"
APP="${1:-ProfessionalVersion.exe}"

for f in oscwus.dll oscsetup.dll oscusb.dll oscusb.so "$APP"; do
    if [ ! -f "$APPDIR/$f" ]; then
        echo "error: $APPDIR/$f not found (run 'make install' first?)" >&2
        exit 1
    fi
done

# Wine binds oscusb.so to the builtin-stamped oscusb.dll only when the
# directory holding them is listed in WINEDLLPATH.
WINEDLLPATH="$APPDIR${WINEDLLPATH:+:$WINEDLLPATH}"
WINEDEBUG="${WINEDEBUG:--all}"
export WINEPREFIX WINEDLLPATH WINEDEBUG

cd "$APPDIR"
exec wine "$APPDIR/$APP"
