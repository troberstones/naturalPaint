#!/usr/bin/env bash
#
# tools/package-linux/package.sh -- turn build/src/naturalPaint into something
# that runs on a machine that is not this one.
#
# The raw build output does not: `find_package(OpenImageIO)` links it against
# whatever prefix CMAKE_PREFIX_PATH pointed at, and that path is baked into the
# binary as an absolute RPATH/RUNPATH. Fine on the box that built it, dead on
# any other -- `error while loading shared libraries: libOpenImageIO.so.3.0:
# cannot open shared object file`. This script copies the binary, figures out
# which of its dependencies are NOT plain system libraries (by asking `ldd`,
# not by hardcoding library names -- an OIIO version bump should not require
# editing this script), bundles just those next to it, strips debug info, and
# repoints it at its own bundled copy instead of the build machine's.
#
# It writes an old-style DT_RPATH, not the DT_RUNPATH patchelf defaults to.
# This is deliberate, not cosmetic: DT_RUNPATH loses to LD_LIBRARY_PATH, and
# LD_LIBRARY_PATH is exactly what DCC tools (Houdini, Nuke, RV -- all commonly
# installed alongside a paint tool) export in a sourced shell, often pointing
# at their OWN bundled OpenImageIO. If that ever lands on the same SONAME as
# ours, DT_RUNPATH silently loses and the wrong .so loads. DT_RPATH is checked
# before LD_LIBRARY_PATH, so it wins regardless. Verified by hand (2026-09-11):
# with a decoy libOpenImageIO.so.3.0 on LD_LIBRARY_PATH, an old-style-RPATH
# binary still resolved to its own bundled lib; the RUNPATH default did not.
# On this box specifically no real collision exists today -- Autodesk RV ships
# libOpenImageIO.so.2.4 (different SONAME) and Houdini renames its copy to
# libOpenImageIO_sidefx.so for exactly this reason -- but nothing guarantees
# the next tool someone installs will bother, so the harder RPATH is the
# default rather than something you opt into after getting bitten once.
#
# See docs/linux-build-plan.md's "Record (RHEL 9.8, no-sudo)" section for the
# from-source OIIO build this was written against, and why a from-source OIIO
# install makes this problem worse than it is on Ubuntu (whose packaged OIIO
# lives under /usr, so find_package never has a non-system prefix to bake in).
#
#   tools/package-linux/package.sh                  # -> build/package/naturalPaint-linux-x86_64/
#   tools/package-linux/package.sh -o dist/np-1.2    # custom output directory
#   tools/package-linux/package.sh --tar             # also write a .tar.gz next to the output dir
#   tools/package-linux/package.sh --no-strip        # keep debug_info (for a debugger, not for shipping)
#
# Requires `patchelf` (not a build dependency of naturalPaint itself, only of
# this script) -- install from your distro, `cargo install patchelf`, or
# https://github.com/NixOS/patchelf if there is no package for it.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$REPO/build"
BIN="$BUILD/src/naturalPaint"

out=""
make_tar=0
do_strip=1

while [ $# -gt 0 ]; do
  case "$1" in
    -o|--out)     out="$2"; shift ;;
    --tar)        make_tar=1 ;;
    --no-strip)   do_strip=0 ;;
    -h|--help)    sed -n '3,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)            printf 'unrecognised argument: %s\n' "$1" >&2; exit 1 ;;
  esac
  shift
done

[ -n "$out" ] || out="$BUILD/package/naturalPaint-linux-x86_64"

say()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m  %s\n' "$*" >&2; exit 1; }

[ -x "$BIN" ] || die "no binary at $BIN -- build first (./start.sh -n will just tell you the same thing)"
command -v patchelf >/dev/null 2>&1 || die "patchelf not found on PATH -- see the header comment for how to get it"

say "packaging $BIN"
rm -rf "$out"
mkdir -p "$out/lib"
cp "$BIN" "$out/naturalPaint"

# --- Runtime resources -------------------------------------------------------
# Whitelisted, not blacklisted: build/src/ also holds CMakeFiles/, Makefile,
# and dev-only tools (flatstest, goldentool) that a distributable has no
# business shipping. These three are the ones src/CMakeLists.txt's POST_BUILD
# step actually stages next to the executable for it to load at runtime.
for res in shaders keymaps third_party; do
  if [ -d "$BUILD/src/$res" ]; then
    cp -r "$BUILD/src/$res" "$out/$res"
  else
    warn "expected resource directory missing: build/src/$res (selftest's \"staged beside the executable\" section will say so too)"
  fi
done

# --- Desktop integration -----------------------------------------------------
# A freedesktop share/ tree, so it can be merged into ~/.local/share or
# /usr/local/share as-is, plus the installer that does that per user with
# Exec= pointed at this package's own binary (a tarball has no fixed path).
if [ -d "$REPO/icons/linux/hicolor" ]; then
  mkdir -p "$out/share/applications" "$out/share/icons"
  cp -r "$REPO/icons/linux/hicolor" "$out/share/icons/hicolor"
  cp "$REPO/icons/linux/naturalPaint.desktop" "$out/share/applications/"
  cp "$REPO/icons/linux/install-desktop-entry.sh" "$out/install-desktop-entry.sh"
  say "desktop entry + icons: share/, install with ./install-desktop-entry.sh"
else
  warn "icons/linux/hicolor missing -- the package will have no launcher entry or icon files"
fi

# --- Find what actually needs bundling ---------------------------------------
# Ask the loader, don't assume: anything ldd resolves outside /lib, /lib64,
# /usr/lib or /usr/lib64 came from a non-system prefix (CMAKE_PREFIX_PATH, an
# rpath into a from-source build tree, etc.) and won't exist on another
# machine. A hardcoded "libOpenImageIO*" list would silently stop bundling the
# day OIIO starts pulling in something else as a shared lib instead of static.
missing=0
bundled=()
while IFS= read -r line; do
  case "$line" in
    *"=> not found")
      libname="${line%% =>*}"
      warn "unresolved at package time: ${libname## }"
      missing=1
      ;;
    *"=>"*)
      libpath="${line#*=> }"
      libpath="${libpath%% (*}"
      [ -f "$libpath" ] || continue
      case "$libpath" in
        /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) continue ;;
      esac
      real="$(readlink -f "$libpath")"
      soname="$(basename "$libpath")"
      realname="$(basename "$real")"
      cp -n "$real" "$out/lib/$realname"
      [ "$soname" = "$realname" ] || ln -sf "$realname" "$out/lib/$soname"
      bundled+=("$soname")
      ;;
  esac
done < <(ldd "$BIN")

[ "$missing" -eq 0 ] || die "the build binary itself has unresolved dependencies -- fix the build before packaging"
[ "${#bundled[@]}" -gt 0 ] || warn "nothing needed bundling -- is OpenImageIO linked from a system path already? (check with: ldd $BIN)"
say "bundled: ${bundled[*]:-<none>}"

# --- Strip and repoint --------------------------------------------------------
# The bundled libs, not just the executable: a from-source OIIO build statically
# absorbs OpenEXR/Imath/OpenColorIO/libjpeg-turbo/WebP/etc into libOpenImageIO.so
# itself, each carrying its own debug_info, none of it stripped by OIIO's own
# build. Measured on this build: libOpenImageIO.so.3.0.18 alone is 167 MB ->
# 15 MB stripped -- skipping this step ships ~150 MB of symbol tables nobody
# asked for, dwarfing what stripping the 21 MB executable saves.
if [ "$do_strip" -eq 1 ]; then
  before=$(stat -c%s "$out/naturalPaint")
  strip "$out/naturalPaint"
  after=$(stat -c%s "$out/naturalPaint")
  say "stripped naturalPaint: $((before / 1024 / 1024)) MB -> $((after / 1024 / 1024)) MB"

  for f in "$out"/lib/*; do
    [ -L "$f" ] && continue   # symlinks (the unversioned SONAME names) -- nothing to strip
    [ -f "$f" ] || continue
    before=$(stat -c%s "$f")
    strip "$f" 2>/dev/null || { warn "strip failed on $(basename "$f"), leaving as-is"; continue; }
    after=$(stat -c%s "$f")
    say "stripped $(basename "$f"): $((before / 1024 / 1024)) MB -> $((after / 1024 / 1024)) MB"
  done
fi

# --force-rpath is the whole point -- see the header comment. Plain --set-rpath
# writes DT_RUNPATH, which is the weaker, environment-overridable form.
patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$out/naturalPaint"
tag=$(readelf -d "$out/naturalPaint" | grep -E "RPATH|RUNPATH" || true)
case "$tag" in
  *"(RPATH)"*) say "rpath: $tag" ;;
  *) die "patchelf did not produce an old-style RPATH -- got: ${tag:-<none>}" ;;
esac

# --- Verify -------------------------------------------------------------------
# Only checks that the loader resolves cleanly in a scrubbed environment --
# NOT a content/regression check. --selftest's pass/fail counts are this
# project's job to gate on (see start.sh --selftest), not this script's; a
# packaged binary that loads and a binary that is functionally correct are two
# different claims.
unresolved=$(env -i ldd "$out/naturalPaint" 2>&1 | grep "not found" || true)
if [ -n "$unresolved" ]; then
  printf '%s\n' "$unresolved" >&2
  die "packaged binary still has unresolved dependencies -- see above"
fi
say "loader check: clean in a scrubbed environment"

if [ "$make_tar" -eq 1 ]; then
  tarball="${out}.tar.gz"
  say "writing $tarball"
  tar czf "$tarball" -C "$(dirname "$out")" "$(basename "$out")"
fi

say "done: $out"
