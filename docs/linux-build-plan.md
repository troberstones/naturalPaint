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
