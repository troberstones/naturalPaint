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
