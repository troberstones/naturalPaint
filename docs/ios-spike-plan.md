# iPad (iOS) build spike — plan

**Goal.** `naturalPaint.app` launches in the iOS Simulator (arm64) from an Xcode
scheme, draws a stroke, and `--selftest` runs to completion and prints a pass/fail
line. The macOS and Linux builds keep producing exactly what they produce today.

**This is a measurement, not a port.** The spike deliberately ships no touch UI, no
device signing and no App Store target. Its output is a filled-in Record section at
the bottom of this file, and a go/no-go on the real thing.

**The three questions it exists to answer**, none of which reading can settle:

1. **Does the OpenImageIO chain cross-compile for iOS, and at what cost?** It is a
   hard requirement — `io/NpaintFile` is multi-part EXR and `io/TileResidency` uses
   `OIIO::ImageCache` as the residency layer for unmodified tiles, so "build without
   it" is not an available answer (see the top-level `CMakeLists.txt` comment that
   removed `NP_USE_OIIO=OFF`). This is the largest unknown and the likeliest place
   the spike dies.
2. **Do the shaders run on Apple's iOS GPU families?** 30 `rgba32float` uses and 78
   `rgba16float`, plus writable storage textures whose per-stage count
   `src/gfx/Context.cpp:174` reads from the adapter and only *prints* — nothing
   adapts to a smaller limit.
3. **What does `--selftest` say on iOS?** The Linux port found 31 failures in five
   root causes, none in the fluid or document code. The equivalent list for iOS is
   the spike's real deliverable.

---

## What is already true (surveyed 2026-09-08 — checked, not assumed)

| finding | evidence |
|---|---|
| wgpu-native ships iOS binaries **for the exact pinned tag** | the v25.0.2.2 release carries `wgpu-ios-aarch64-release.zip` (12.8 MB) plus arm64 and x86_64 simulator slices |
| Apple Pencil arrives as `SDL_PenEvent` with no work from us | vendored SDL 3.2.24's `src/video/uikit/SDL_uikitpen.m` reports `PRESSURE \| XTILT \| YTILT`, plus `ROTATION` (Pencil Pro barrel roll) and `DISTANCE` (hover). `app/PenAxes` already consumes `SDL_PEN_AXIS_ROTATION` |
| surface creation should compile unchanged | `src/gfx/Context.cpp:53` uses `SDL_Metal_CreateView`/`SDL_Metal_GetLayer`; UIKit implements both in `SDL_uikitmetalview.m` |
| the text shaper is iOS-clean | `src/text/CoreTextShaper.mm` imports only CoreText, CoreGraphics and CoreFoundation — no AppKit |
| the threading layer is iOS-clean | `src/core/Parallel.hpp` is `dispatch_apply` from libSystem |
| the memory probe is iOS-clean | `src/app/Memory.cpp` is mach `task_info`; `phys_footprint` exists on iOS |
| resource loading already has the right tier | `src/core/ResourcePaths.cpp` tier 2 is executable-adjacent, which on iOS *is* the `.app` bundle root. Tier 3's compile-time source path will not exist on device, so tier 2 wins by construction |
| user data paths already resolve inside the sandbox | the seven `getenv("HOME")` sites build `$HOME/Library/Application Support/naturalPaint/...`; on iOS `HOME` is the app container, which has that directory |
| the menus survive losing the native bar | `src/ui/MacPaintUI.cpp:12756` already draws an in-window `BeginMainMenuBar()`; `ui/MacNativeMenu` is additive |
| `--selftest` can write its PNG somewhere writable with no code change | `src/main.cpp:1600` — `--selftest <path>` takes an optional output path, so the scheme passes one in the container's `Documents/` |
| the platform seams already exist | the Linux port cut them (`docs/linux-build-plan.md`); only 32 `__APPLE__` sites in 23 files |

## What blocks it

| blocker | where | fix | est. |
|---|---|---|---|
| **OpenImageIO is a macOS `.dylib` chain** — the local 3.1.16 build pulls OpenEXR/Imath/IlmThread/Iex, libtiff, jpeg-turbo, libpng, giflib, zlib **and OpenColorIO**, which drags in yaml-cpp/pystring/expat/minizip-ng. iOS needs all of it static, arm64-ios | top-level `CMakeLists.txt`, `find_package(OpenImageIO REQUIRED)` at `src/CMakeLists.txt:653` | see **Phase 1** and its stop rule — this is the time-boxed part | 1–3 d |
| **`__APPLE__` is true on iOS**, so all 32 guards currently mean *macOS* and would compile AppKit into an iOS build | 23 files | introduce `NP_PLATFORM_MACOS` / `NP_PLATFORM_IOS` from `TARGET_OS_OSX`/`TARGET_OS_IPHONE` in one header; the guards that are genuinely "any Apple" (Parallel, Memory, ResourcePaths, CoreText) stay `__APPLE__` | 1 d |
| **three AppKit `.mm` files cannot build for iOS** | `ui/MacNativeMenu.mm`, `ui/MacTrackpadTouch.mm`, `ui/MacTrackpadGestures.mm` | excluded from the iOS target; their headers already ship non-Apple inline stubs from the Linux port, which is the seam to reuse | hours |
| **no `main()` entry point** — iOS needs `SDL_UIKitRunApp` | `src/main.cpp:1382` | `#include <SDL3/SDL_main.h>` | minutes |
| **no file dialog backend for UIKit** — SDL's `src/dialog/` has cocoa/android/unix/windows/haiku only | five callers of `SDL_ShowFileDialogWithProperties`, behind `src/ui/FileDialog.hpp` | **spike: stub it** and record what the stub costs the suite. A real `UIDocumentPickerViewController` backend is port work, not spike work | hours (stub) |
| **font table lists macOS and Linux paths only** | `src/ui/Fonts.cpp:33` | add the bundled face; Lucide is already a bundled resource | hours |
| **resources are not in a bundle** | `src/CMakeLists.txt:608` stages beside the binary | same staging, into the `.app` — `shaders/`, `keymaps/`, the Mixbox LUT, `lucide.ttf`, `codepoints.json` | hours |
| **app icon has no target to attach to** — the 1024 px `icons/ios/AppIcon.appiconset` is generated by `icons/make_icons.sh` and committed, but nothing references it | `icons/ios/` | add the set to the iOS target's asset catalog (`ASSETCATALOG_COMPILER_APPICON_NAME=AppIcon`). `ui/AppIcon`'s runtime `SDL_SetWindowIcon` is a no-op on UIKit, so this is the only route | minutes |
| **app lifecycle** — iOS terminates a process that touches the GPU while backgrounded | `src/main.cpp` main loop | handle `SDL_EVENT_WILL_ENTER_BACKGROUND`/`DID_ENTER_FOREGROUND`: stop submitting, drop and reconfigure the surface | 0.5 d |
| **two extra executables have no meaning on iOS** | `goldentool`, `flatstest` | not in the iOS target; they stay macOS/Linux tools | minutes |

## Phases

**Phase 0 — toolchain skeleton (half a day).** `CMAKE_SYSTEM_NAME=iOS` with the Xcode
generator, simulator SDK, arm64 only. Add the iOS branch to
`cmake/Dependencies.cmake` — same `file(DOWNLOAD ... EXPECTED_HASH)` + hash-the-`.a`
pattern the Linux branch already uses, against
`wgpu-ios-aarch64-simulator-release.zip`, and re-derive the pin with `shasum -a 256`
rather than copying one. SDL3 builds for iOS from the same `FetchContent`. Prove the
skeleton by building a two-line SDL+wgpu program in the simulator **before** touching
naturalPaint's own sources.

**Phase 1 — OpenImageIO, time-boxed to two days.** Attempt in this order, stopping at
the first that works:

1. **vcpkg `arm64-ios` triplet** for `openimageio` and its chain. Fastest if the port
   works; the ports are not iOS-tested, so assume some patching.
2. **Hand-built static chain**: Imath → OpenEXR → zlib/libpng/libjpeg-turbo/libtiff →
   (OpenColorIO, if OIIO 3.1 still requires it) → OpenImageIO, each with
   `BUILD_SHARED_LIBS=OFF` and the same iOS toolchain file. This is the honest
   fallback and the reason for the time box.
3. **Stop and record.** If neither lands in two days, the spike's answer to question 1
   is "expensive, here is exactly where it broke" — which is a *successful* spike, not
   a failed one. Do not start rewriting `io/` to avoid OIIO: `ImageCache` is the tile
   residency layer, so that is a redesign, not a workaround.

**Phase 2 — make it link and launch.** The five mechanical blockers above (platform
split, `.mm` exclusion, `SDL_main.h`, dialog stub, bundle resources), then a fix loop
over whatever the compiler and linker say. Exit: the window appears in the simulator,
the chrome draws, and a mouse-as-touch drag deposits paint.

**Phase 3 — make `--selftest` run.** Scheme argument `--selftest
<container>/Documents/selftest.png`. Triage the failures into the same five buckets
the Linux port used, and — per PLAN.md §1.5 — where a platform's answer genuinely
differs, print *that platform's* answer under its own label rather than `#ifdef`-ing
an assertion out. Expect trouble in at least: idle RSS (`selftest/IdleMemory`), format
support (`selftest/FormatSupport` asks the linked OIIO, so it should adapt on its own —
verify rather than assume), the file dialog sections against a stub, and anything
that asserts a `/tmp` path is writable.

**Phase 4 — record.** Fill the Record section below with what was found, in the shape
`docs/linux-build-plan.md`'s record uses: what the survey could not see without a
compiler.

## Explicitly out of scope, and why

- **The touch UI.** 51 `IsItemHovered`, 28 `SetTooltip`, 8 right-click popups, 49 key
  bindings, a 36 px title bar against Apple's 44 pt minimum. That is a second
  front-end, not a port, and the spike must not be allowed to turn into it. The
  simulator drives touch-as-mouse, which is enough to answer the three questions.
- **A real file-dialog backend**, device signing, and the App Store target.
- **The golden harness.** `tools/golden/run_golden.sh` is not run by `--selftest`
  anyway, and its capture path is macOS-shaped.
- **Memory budget tuning.** The solver is already capped independently of document
  size (`kPaintSimMaxTexels`, 293 B/texel worst case inside a 512 MB budget —
  `src/app/selftest/SolverFootprint.cpp:135`), which is the right architecture. But
  512 MB of solver plus the tile store plus wgpu's ledger will meet jetsam on
  anything below an M-series iPad, so a real port sizes that budget from device RAM.
  Record the observed footprint; change nothing.
- **The Mixbox licence.** `NP_USE_MIXBOX` is `ON` by default and Mixbox is CC BY-NC,
  so a commercial App Store build must either license it or run the KM fallback —
  and by this repo's own §1.5 argument that OFF path is unexercised, so it is
  probably not green. A shipping decision, not a spike blocker.

## House rules that apply

`.claude/AGENT-BRIEF.md`: flat `namespace np`; explicit CMake source lists; a design
rationale lives once, in the header of the module that owns it; no change to existing
`--selftest` output lines except where a line asserted a platform limitation that no
longer holds; zero warnings naming a `src/` path. Capture stdout and stderr to
separate files, never merged.

---

## Record

*(empty — fill in Phase 4)*
