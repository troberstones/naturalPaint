#!/usr/bin/env bash
# icons/linux/install-desktop-entry.sh -- make a built naturalPaint appear in
# the Linux desktop's app launcher, with its icon, for the current user.
#
#   icons/linux/install-desktop-entry.sh [path/to/naturalPaint]   (default: build/src/naturalPaint)
#   icons/linux/install-desktop-entry.sh --uninstall
#
# tools/package-linux/package.sh ships this at the package root, beside the
# binary and a share/ tree; run from there it defaults to that binary. Exec= is
# absolute, so rerun it after moving the package.
#
# Installs, under ${XDG_DATA_HOME:-~/.local/share}:
#   icons/hicolor/<N>x<N>/apps/naturalPaint.png   (16..512, from icons/linux/hicolor)
#   applications/naturalPaint.desktop             (Exec= rewritten to the binary's absolute path)
#
# Everything is named "naturalPaint" because that is SDL's default app id (the
# executable's name), which compositors match to the desktop entry.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
NAME="naturalPaint"

if [[ -d "$HERE/share/icons/hicolor" ]]; then  # a package from package.sh
  ICONS="$HERE/share/icons/hicolor"
  DESKTOP="$HERE/share/applications/$NAME.desktop"
  DEFAULT_BIN="$HERE/$NAME"
else                                            # the source tree's icons/linux
  ICONS="$HERE/hicolor"
  DESKTOP="$HERE/$NAME.desktop"
  DEFAULT_BIN="$(cd "$HERE/../.." && pwd)/build/src/$NAME"
fi

if [[ "${1:-}" == "--uninstall" ]]; then
  rm -f "$DATA/applications/$NAME.desktop"
  for size in 16 24 32 48 64 128 256 512; do
    rm -f "$DATA/icons/hicolor/${size}x${size}/apps/$NAME.png"
  done
  echo "removed $NAME's desktop entry and icons from $DATA"
  exit 0
fi

BIN="${1:-$DEFAULT_BIN}"
if [[ ! -x "$BIN" ]]; then
  echo "error: no executable at '$BIN' -- build first, or pass its path." >&2
  exit 1
fi
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

for size in 16 24 32 48 64 128 256 512; do
  mkdir -p "$DATA/icons/hicolor/${size}x${size}/apps"
  cp "$ICONS/${size}x${size}/apps/$NAME.png" "$DATA/icons/hicolor/${size}x${size}/apps/"
done

mkdir -p "$DATA/applications"
# Desktop Entry Exec quoting: escape " ` $ \ and double %, then double every
# backslash again for the string value. Not sed: it reinterprets & | \.
EXEC_PATH="${BIN//\\/\\\\}"
EXEC_PATH="${EXEC_PATH//\"/\\\"}"
EXEC_PATH="${EXEC_PATH//\`/\\\`}"
EXEC_PATH="${EXEC_PATH//\$/\\\$}"
EXEC_PATH="${EXEC_PATH//%/%%}"
EXEC_PATH="${EXEC_PATH//\\/\\\\}"
while IFS= read -r line || [[ -n "$line" ]]; do
  if [[ "$line" == Exec=* ]]; then
    printf 'Exec="%s" %%F\n' "$EXEC_PATH"
  else
    printf '%s\n' "$line"
  fi
done < "$DESKTOP" > "$DATA/applications/$NAME.desktop"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$DATA/icons/hicolor" || true

echo "installed $DATA/applications/$NAME.desktop -> $BIN"
