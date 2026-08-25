#!/usr/bin/env bash
# Install the WTSN Configurator desktop launcher onto this user's Desktop.
set -e
SRC="$(cd "$(dirname "$0")" && pwd)/WTSN Configurator.desktop"
DST="$HOME/Desktop/WTSN Configurator.desktop"
cp "$SRC" "$DST"
chmod +x "$DST"
if command -v gio >/dev/null 2>&1; then
    gio set "$DST" metadata::trusted true 2>/dev/null || true
fi
echo "Installed launcher: $DST"
echo "Double-click the 'WTSN Configurator' icon to start."
