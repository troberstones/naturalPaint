#!/usr/bin/env bash
# icons/linux/install-desktop-entry.sh -- make a built naturalPaint appear in
# the Linux desktop's app launcher, with its icon, for the current user.
#
#   icons/linux/install-desktop-entry.sh [path/to/naturalPaint]   (default: build/src/naturalPaint)
#   icons/linux/install-desktop-entry.sh --uninstall
#
# Installs, under ${XDG_DATA_HOME:-~/.local/share}:
#   icons/hicolor/<N>x<N>/apps/naturalPaint.png   (16..512, from icons/linux/hicolor)
#   applications/naturalPaint.desktop             (Exec= rewritten to the binary's absolute path)
#
# The desktop file's name, its Icon= and its StartupWMClass= are all
# "naturalPaint" because that is the app id SDL reports to the compositor
# (SDL_GetAppID(): the executable's name, since the app sets no identifier)
# -- Wayland compositors and X11 taskbars match a running window to its
# desktop entry, and so to its icon, by exactly that string.
#
# Per-user and outside the build on purpose: this build has no install()
# layout (the binary finds its shaders and keymaps beside itself or in the
# source tree -- core/ResourcePaths.hpp), so a system-wide install would have
# nothing coherent to point Exec= at.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
NAME="naturalPaint"

if [[ "${1:-}" == "--uninstall" ]]; then
  rm -f "$DATA/applications/$NAME.desktop"
  for size in 16 24 32 48 64 128 256 512; do
    rm -f "$DATA/icons/hicolor/${size}x${size}/apps/$NAME.png"
  done
  echo "removed $NAME's desktop entry and icons from $DATA"
  exit 0
fi

BIN="${1:-$ROOT/build/src/naturalPaint}"
if [[ ! -x "$BIN" ]]; then
  echo "error: no executable at '$BIN' -- build first, or pass its path." >&2
  exit 1
fi
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

for size in 16 24 32 48 64 128 256 512; do
  mkdir -p "$DATA/icons/hicolor/${size}x${size}/apps"
  cp "$HERE/hicolor/${size}x${size}/apps/$NAME.png" "$DATA/icons/hicolor/${size}x${size}/apps/"
done

mkdir -p "$DATA/applications"
# The Exec key's own rules (Desktop Entry spec, "The Exec key"), so a checkout
# under any path still launches: inside the quotes, " ` $ and \ are
# backslash-escaped and % doubled; then the value's string escaping doubles
# every backslash once more. Written line by line, not with sed, whose
# replacement text would itself reinterpret & | and \.
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
done < "$HERE/$NAME.desktop" > "$DATA/applications/$NAME.desktop"

# Both refreshes are optional conveniences; most desktops notice on their own.
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA/applications" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$DATA/icons/hicolor" || true

echo "installed $DATA/applications/$NAME.desktop -> $BIN"
