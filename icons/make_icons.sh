#!/usr/bin/env bash
# icons/make_icons.sh -- regenerate every platform's app icon from the master.
#
#   icons/make_icons.sh [master.png]      (default: icons/np_icon.png)
#
# The master is a 1024 px export of icons/np_icon.kra. The outputs are committed
# so a build needs neither ImageMagick nor iconutil. Without ImageMagick, sips
# resizes and Python packs the .ico from PNG entries (readable since Vista).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MASTER="${1:-$HERE/np_icon.png}"
NAME="naturalPaint"  # also the SDL app id on Linux (SDL_GetAppID(): the exe name)

if [[ ! -f "$MASTER" ]]; then
  echo "Usage: $0 [path-to-master-1024x1024.png]" >&2
  exit 1
fi

HAS_MAGICK=false
command -v magick >/dev/null 2>&1 && HAS_MAGICK=true
HAS_SIPS=false
command -v sips >/dev/null 2>&1 && HAS_SIPS=true
if [[ "$HAS_MAGICK" != true && "$HAS_SIPS" != true ]]; then
  echo "Error: need ImageMagick (magick) or macOS sips to resize." >&2
  exit 1
fi

resize_image() {  # src dim dest
  if [[ "$HAS_MAGICK" == true ]]; then
    magick "$1" -resize "${2}x${2}" "$3"
  else
    sips -z "$2" "$2" "$1" --out "$3" >/dev/null
  fi
}

echo "==> master: $MASTER"

# Generated files only: hand-written ones live beside them in each directory.
rm -rf "$HERE/macos/$NAME.icns" "$HERE/linux/hicolor" "$HERE/ios" "$HERE/windows/$NAME.ico"
mkdir -p "$HERE/macos" "$HERE/windows" "$HERE/linux" "$HERE/ios"

# -----------------------------------------------------------------------------
# 1. macOS (.icns) -- the .app bundle's CFBundleIconFile (NP_MACOS_APP_BUNDLE)
# -----------------------------------------------------------------------------
if command -v iconutil >/dev/null 2>&1; then
  echo "==> macOS .icns"
  ICONSET="$(mktemp -d)/$NAME.iconset"
  mkdir -p "$ICONSET"
  for entry in 16:icon_16x16.png 32:icon_16x16@2x.png 32:icon_32x32.png \
               64:icon_32x32@2x.png 128:icon_128x128.png 256:icon_128x128@2x.png \
               256:icon_256x256.png 512:icon_256x256@2x.png 512:icon_512x512.png \
               1024:icon_512x512@2x.png; do
    resize_image "$MASTER" "${entry%%:*}" "$ICONSET/${entry##*:}"
  done
  iconutil -c icns "$ICONSET" -o "$HERE/macos/$NAME.icns"
  rm -rf "$(dirname "$ICONSET")"
else
  echo "==> skipping macOS .icns: iconutil not found (run on macOS)"
fi

# -----------------------------------------------------------------------------
# 2. Linux (freedesktop hicolor) -- also the source of the PNG compiled into
#    the binary for the runtime window icon (ui/AppIcon, the 512 px one)
# -----------------------------------------------------------------------------
echo "==> Linux hicolor"
for size in 16 24 32 48 64 128 256 512; do
  dest="$HERE/linux/hicolor/${size}x${size}/apps"
  mkdir -p "$dest"
  resize_image "$MASTER" "$size" "$dest/$NAME.png"
done

# -----------------------------------------------------------------------------
# 3. Windows (.ico) -- embedded in the .exe by icons/windows/naturalPaint.rc.
#    Packed from the hicolor PNGs above when ImageMagick is absent.
# -----------------------------------------------------------------------------
echo "==> Windows .ico"
if [[ "$HAS_MAGICK" == true ]]; then
  magick "$MASTER" -define icon:auto-resize=256,128,64,48,32,24,16 "$HERE/windows/$NAME.ico"
else
  python3 - "$HERE/windows/$NAME.ico" "$HERE/linux/hicolor" <<'PY'
import struct, sys, os
out, root = sys.argv[1], sys.argv[2]
sizes = [256, 128, 64, 48, 32, 24, 16]
blobs = [open(os.path.join(root, f"{s}x{s}", "apps", "naturalPaint.png"), "rb").read() for s in sizes]
header = struct.pack("<HHH", 0, 1, len(sizes))
offset = 6 + 16 * len(sizes)
entries = b""
for s, b in zip(sizes, blobs):
    dim = 0 if s == 256 else s  # 0 means 256 in an ICONDIRENTRY
    entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(b), offset)
    offset += len(b)
with open(out, "wb") as f:
    f.write(header + entries + b"".join(blobs))
PY
fi

# -----------------------------------------------------------------------------
# 4. iOS (asset catalog) -- not wired to anything yet: there is no iOS target
#    (docs/ios-spike-plan.md). App Store icons may not carry alpha.
# -----------------------------------------------------------------------------
echo "==> iOS AppIcon.appiconset"
IOS_DIR="$HERE/ios/AppIcon.appiconset"
mkdir -p "$IOS_DIR"
if [[ "$HAS_MAGICK" == true ]]; then
  magick "$MASTER" -background white -alpha remove -alpha off -resize 1024x1024 "$IOS_DIR/icon-1024.png"
else
  # sips keeps the alpha channel; this master is fully opaque, so the channel
  # carries nothing, but a master WITH transparency needs ImageMagick here.
  resize_image "$MASTER" 1024 "$IOS_DIR/icon-1024.png"
fi
cat > "$IOS_DIR/Contents.json" <<'EOF'
{
  "images" : [
    {
      "size" : "1024x1024",
      "idiom" : "universal",
      "platform" : "ios",
      "filename" : "icon-1024.png"
    }
  ],
  "info" : {
    "version" : 1,
    "author" : "xcode"
  }
}
EOF

echo "==> done: icons/{macos,windows,linux/hicolor,ios}"
