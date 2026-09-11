# Linux build — plan

**Goal.** `cmake -S . -B build && cmake --build build` on Ubuntu 24.04 (x86_64, g++ 13)
produces `build/src/naturalPaint`, and `xvfb-run ./build/src/naturalPaint --selftest`
runs the suite against a software Vulkan adapter (Mesa lavapipe). The macOS build must
keep producing exactly what it produces today.

**What actually blocks it** (surveyed 2026-09-04, not assumed):

| blocker | where | fix |
|---|---|---|
| `wgpu-native` is a vendored macOS-arm64 `.a` with a pinned SHA | `cmake/Dependencies.cmake` | select by platform; on non-Apple, `file(DOWNLOAD)` the `wgpu-linux-x86_64-release.zip` for the same `v25.0.2.2` tag into the build tree with an `EXPECTED_HASH`, extract, and import that `.a`; link `dl`, `pthread`, `m` |
| surface creation is Metal-only, `#error` elsewhere | `src/gfx/Context.cpp` | Linux branch: `WGPUSurfaceSourceXlibWindow` from SDL3's X11 window properties, `WGPUSurfaceSourceWaylandSurface` from the Wayland ones, chosen by `SDL_GetCurrentVideoDriver()` |
| idle-RSS measurement uses `<mach/...>` | `src/app/Memory.cpp` | Linux: `/proc/self/statm` resident pages × page size |
| executable path uses `_NSGetExecutablePath` | `src/core/ResourcePaths.cpp` | Linux: `readlink("/proc/self/exe")` |
| OpenImageIO must be found | top-level `CMakeLists.txt` | Ubuntu's `libopenimageio-dev` (2.4) provides the CMake config; no change expected |
| every other `__APPLE__` guard | 9 files | already carries an `#else` branch; no change expected |

The `.mm` translation units are already `if(APPLE)`-gated with `text/StubShaper.cpp` on
the other side, and the three Mac-only headers already ship non-Apple inline stubs.

**Phases.**

1. **Make it link** — the four fixes above, each on disjoint files, in parallel; then one
   build here, and a fix loop over whatever the compiler and linker say.
2. **Make `--selftest` run** under `xvfb-run` with lavapipe. Sections that encode a
   macOS-only answer (paths, the shaper stub, fonts) get the correct answer for both
   platforms stated, never an `#ifdef`'d-out assertion (PLAN.md §1.5).
3. **Record it** — README build section for Linux, and this file's table becomes the
   "what changed" record.

**House rules that apply** (`.claude/AGENT-BRIEF.md`): flat `namespace np`; explicit
CMake source lists; a design rationale lives once, in the header of the module that
owns it; no change to existing `--selftest` output lines except where the line asserted
a platform limitation that no longer holds; zero warnings naming a `src/` path.

---

## Record (2026-09-04)

**Phase 1 — it links.** Configure, build and link succeed on Ubuntu 24.04 x86_64
with g++ 13, Ninja, Ubuntu's `libopenimageio-dev` 2.4 and the `wgpu-native`
`v25.0.2.2` Linux release fetched at configure time. Beyond the table above, the
build surfaced four more things the survey could not see without a compiler:

| found by | where | what |
|---|---|---|
| configure | `src/CMakeLists.txt` | Ubuntu's OpenImageIO CMake config exports an OpenCV include path that is not installed; CMake refuses to generate against a missing interface directory, so absent paths are dropped |
| configure | environment | `openimageio-tools` must be installed: the dev package's config declares imported executable targets whose binaries live in that package |
| compile | `app/OpenAnyFile.cpp`, `app/selftest/RecoveryJournal.cpp`, `io/OiioBackend.cpp` | a missing `<cmath>`; `F_FULLFSYNC` (macOS only — `syncfs` is the Linux measurement); OpenImageIO 2.4's raw-pointer `ImageCache::create()` |
| run | `main.cpp`, `ui/Fonts.cpp` | `SDL_WINDOW_METAL` refused by the x11 driver; font tables listed only macOS paths |

**Phase 2 — the suite runs.** `xvfb-run -a ./build/src/naturalPaint --selftest` on
Mesa llvmpipe: **8040 pass, 31 fail, 1003 s wall** (macOS: ~5 s on real hardware).
The failures fell into five root causes, none of them in the fluid or document code:

1. OpenImageIO **decode** through the 2.4 in-memory read path (EXR, TIFF, DPX, HDR
   and PSD round trips, open-any-file, PSD import, one tile-residency wording). Two
   root causes in `io/OiioBackend.cpp`: 2.4 treats an extension-less name with the
   proxy given only through the config attribute as a missing file (fixed by passing
   the proxy through `ImageInput::open`'s own parameter), and its PSD reader claims
   proxy support yet reads past a memory proxy's end (fixed by retrying a refused
   proxy open through a temporary file). The ImageCache's spec reports an untiled
   source as one full-size tile, so the native tile shape now comes from a direct
   header open.
2. `core/Half` on x86-64 needs `-mf16c` to take its hardware path; the flood edge-band
   figure moves with it. Fixed in `src/CMakeLists.txt`.
3. Ubuntu's OpenImageIO carries **LibRaw**, so camera raw is supported here and
   deliberately not on macOS; the format-support section now has to ask OpenImageIO
   rather than assert one build's list.
4. Measurements whose Linux answer differs: idle RSS (llvmpipe's LLVM and Ubuntu's
   OpenImageIO chain are resident before this build allocates anything), the adaptive
   trickle budget on a 10 ms/tile software GPU, and a journal write failure provoked
   by permissions that root ignores.
5. The Text layer's typing check assumed a shaper; it now states the stub's answer.

**Phase 2 — result.** After the fixes above, on the same box:
**8078 pass, 0 fail, 145 sections, zero warnings, exit 0, 937 s wall** (`--selftest`
also wrote its output PNG). Every macOS-printed line is unchanged; where a platform's
answer differs the section prints the Linux one under its own label, and the two
figures that vary with the machine (idle RSS allowances, the trickle rate) carry
`[measured]`.

Two things worth knowing before the next person touches this:

- **A copied build directory is not isolated.** `cp -r build build-x` keeps absolute
  paths to `build/` in the copy's cache and Ninja files, and building the copy
  reconfigures the original. Use a fresh `cmake -S . -B build-x` instead.
- **The suite is slow here because llvmpipe is a CPU rasteriser**, not because
  anything regressed: the GPU sections dominate the 15 minutes. On a machine with a
  real Vulkan device it should approach the macOS figure.

---

## Record (RHEL 9.8, no-sudo, 2026-09-11)

Same source tree, a different Linux entirely from the one above: RHEL 9.8
("Plow"), no sudo (no package installs, period), and no `OpenImageIO-devel`
RPM at all -- the Ubuntu record's "packaged OIIO is enough" does not hold here.
Recorded because the fixes needed are different in kind, not degree, from
Phase 1/2 above.

**Building OpenImageIO with no package manager access.** Built from source into
a user-owned prefix, letting CMake fetch OIIO's own missing deps (OpenEXR,
Imath, OpenColorIO, fmt, pugixml, robin-map) rather than trying to source them
individually -- none of those have devel RPMs here either.

**GCC 11.5 cannot build this tree.** `src/core/Half.hpp` needs `_Float16`,
which is a GCC 12 feature; RHEL 9.8's system GCC is 11.5 and there is no sudo
to install a newer one. Fix used: build both OpenImageIO and naturalPaint with
zig's bundled Clang (`zig cc` / `zig c++`) instead, with `CC`/`CXX` propagated
into OIIO's own dependency sub-builds so every library in the chain shares one
compiler and one C++ ABI. A separately-downloaded standalone LLVM toolchain
also works and was tried first, but at 12 GB for a box that already had zig
installed, it is strictly worse -- keeping it around a single build here for
comparison purposes was not worth the disk.

**Result.** Configures, builds, and `--selftest` runs to completion. Six
sections fail here that pass on the Ubuntu/llvmpipe box above --
`fonts`, `no-document canvas`, `inpaint` (this one is flaky -- not consistent
run to run), `move tool`, `apply pass`, `panel settings` -- and have not been
root-caused; recorded as an open question for whoever next builds on a
GCC-11/RHEL9/NVIDIA-proprietary-driver box like this one, not asserted to be
this environment's fault.

**The packaging problem this environment exposes that Ubuntu's does not.**
`find_package(OpenImageIO)` bakes `CMAKE_PREFIX_PATH` into the binary as an
absolute RPATH. On Ubuntu that path is `/usr` (system package), invisible
because it is already everywhere. Here it is `/work/.../openimageio-zig/lib64`
-- a path that exists on exactly one machine. `build/src/naturalPaint` as
CMake produces it is therefore **not the thing to hand to anyone else**, on
this kind of build; copying it to a different directory on the *same* machine
is enough to break it. `tools/package-linux/package.sh` fixes this: it
bundles the non-system shared libraries `ldd` actually finds (found by
inspecting the loader's output, not by hardcoding OpenImageIO's name -- so a
version bump doesn't require editing the script), strips debug info from
*both* the executable and the bundled libraries, and repoints the binary at
its own bundled copy with an old-style `DT_RPATH` rather than `patchelf`'s
default. Stripping the libraries matters as much as stripping the executable:
a from-source OIIO build with `EMBEDPLUGINS=ON` statically absorbs OpenEXR,
Imath, OpenColorIO, libjpeg-turbo, WebP and others into `libOpenImageIO.so`
itself, each carrying its own unstripped debug info, so on this build
`libOpenImageIO.so.3.0.18` alone measured 167 MB -> 14 MB stripped and
`libOpenImageIO_Util.so` 18 MB -> 1 MB -- dwarfing the executable's own
234 MB -> 21 MB. A version of this script that only stripped the executable
would have shipped a ~230 MB package; stripping both brings it to ~39 MB, with
`--selftest`'s known-failure set unchanged before and after. `USE_QT=ON` in
the OIIO build does not contribute to this: it only builds OIIO's own `iv`
viewer tool (not something naturalPaint links against), confirmed by zero Qt
symbols in `libOpenImageIO.so` itself.

That distinction (`DT_RPATH` over `patchelf`'s default `DT_RUNPATH`) is
load-bearing, not cosmetic: `DT_RUNPATH`
loses to `LD_LIBRARY_PATH`, and `LD_LIBRARY_PATH` is exactly what gets
exported by a sourced Houdini/Nuke/RV environment on a box that also has DCC
tools installed -- several of which bundle their own OpenImageIO. Verified by
hand with a decoy `libOpenImageIO.so.3.0` on `LD_LIBRARY_PATH`: the
`DT_RUNPATH` default lost to it, the forced `DT_RPATH` did not. No such
collision exists on this box today (Autodesk RV ships a different SONAME,
`libOpenImageIO.so.2.4`; Houdini renames its copy to
`libOpenImageIO_sidefx.so` for this exact reason), but nothing guarantees the
next tool installed alongside naturalPaint will bother to.

One thing worth knowing before the next person touches this: **always package
before handing a binary to anyone, even for a same-machine test in a different
directory** -- `build/src/naturalPaint` carries an absolute path to this
machine's OIIO prefix and will not run once moved. `tools/package-linux/package.sh`
is the only supported way to produce something relocatable.
