# Building and testing on a physical iPad

**This is about a real, connected iPad — not the simulator.** For the
simulator, use the `mcp__Claude_Code_iOS_Simulator__control` tool directly;
it cannot see or drive a physical device at all. Everything below uses
`xcrun devicectl`, which is the opposite: it only talks to real hardware over
its CoreDevice tunnel, never a simulator.

This complements `docs/ios-spike-plan.md` (the original build spike, which
predates a working device build) — as of 2026-09-17 a signed device build
exists and was used to build, install and run a headless CLI flag on a real
iPad Pro (M5) from this machine. If you hit something this doc doesn't cover,
that plan doc's blockers table is the place to check next.

## Prerequisites (already true on this machine, verify before assuming)

- `arm64-ios` (device, **not** `arm64-ios-simulator`) OpenImageIO chain built
  via vcpkg at `~/vcpkg/installed/arm64-ios`. Check with
  `ls ~/vcpkg/installed/arm64-ios` — if missing, that's Phase 1 of
  `docs/ios-spike-plan.md`, not a quick fix.
- `third_party/mixbox` submodule initialized
  (`git submodule update --init third_party/mixbox` — see
  `naturalpaint-worktree-mixbox-submodule` if working from a fresh worktree).
- A physical device connected and trusted. Check with:
  ```bash
  xcrun devicectl list devices
  ```
  This prints its `Identifier` (a UDID) and `State` (`connected`). If it says
  anything else, the device needs to be woken/unlocked/re-paired — nothing
  below will work until `devicectl` sees `connected`.
- An Apple Developer team with a working automatic-signing setup for this
  bundle id (`io.github.troberstones.naturalPaint`). If a `build-ios-device`
  directory already exists anywhere on the machine (check the main checkout,
  not just your worktree), its `CMakeCache.txt` has the exact team IDs that
  already work — copy them rather than guessing:
  ```bash
  grep -E "DEVELOPMENT_TEAM" /Users/chrisharvey/naturalPaint/build-ios-device/CMakeCache.txt
  ```

## Configuring a build

**If working in a git worktree** (not the main checkout), configuring from
scratch re-fetches SDL3/Dear ImGui via CMake `FetchContent`, which is slow.
Point at the main checkout's already-fetched sources instead — this is the
same trick `naturalpaint-worktree-fast-configure` records for the macOS
build, and it works here too (source-only, read-only, safe to share):

```bash
cmake -S . -B build-ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team id from CMakeCache.txt above> \
  -DNP_IOS_OIIO_PREFIX=/Users/chrisharvey/vcpkg/installed/arm64-ios \
  -DFETCHCONTENT_SOURCE_DIR_SDL3=/Users/chrisharvey/naturalPaint/build-ios-device/_deps/sdl3-src \
  -DFETCHCONTENT_SOURCE_DIR_IMGUI=/Users/chrisharvey/naturalPaint/build-ios-device/_deps/imgui-src
```

Note this is `iphoneos` (device) sysroot, not `iphonesimulator` — the two are
different SDKs and produce binaries that cannot run on the other target.
Measured: ~3.5 minutes for this configure (vs. ~9+ minutes cold), on this
machine, with `_deps` reused from an existing device build. If no prior
`build-ios-device` exists anywhere to borrow `_deps`/team ID from, drop the
two `FETCHCONTENT_SOURCE_DIR_*` flags (slower, but still works) and get the
team ID from Xcode's own signing settings instead.

## Building

```bash
cmake --build build-ios-device --config Debug
```

Ninja is not used for the iOS target — this is a generated Xcode project, so
the build step invokes `xcodebuild` under the hood. Expect a lot of benign
linker warnings (deployment-target mismatches from vcpkg's own build
timestamps, duplicate OpenColorIO symbols across debug/release variants it
links defensively) — these are pre-existing and not a sign of a broken
build. Watch for `** BUILD SUCCEEDED **` at the end. Ccache (if configured,
check `NP_CCACHE` in the cache) makes a second build of an already-built tree
very fast — a no-op rebuild here took well under a minute.

The signed `.app` lands at:
```
build-ios-device/src/Debug-iphoneos/naturalPaint.app
```

## Installing and running on the device

Get the device's identifier once (reuse it, don't re-look-it-up per command):
```bash
xcrun devicectl list devices
```

Install:
```bash
xcrun devicectl device install app --device <UDID> build-ios-device/src/Debug-iphoneos/naturalPaint.app
```

Run a headless CLI flag and capture its stdout/stderr, waiting for it to exit
(`--console` is what makes this block and stream output back; without it,
`launch` returns immediately and you see nothing):
```bash
xcrun devicectl device process launch --device <UDID> --console --terminate-existing \
  io.github.troberstones.naturalPaint -- --profile-flats-solver 2048 2048 5
```

`--terminate-existing` kills any prior running instance first — worth always
including, since a previous run's process can otherwise still be alive and
block the next launch. The command exits with the app's own exit code
reported as `The app terminated with the exit code N.` — check that line,
not just whether `devicectl` itself returned 0.

This same pattern works for **any** headless CLI flag this project has
(`--selftest`, `--flats-gpu-check`, `--profile-vector-warp`, etc.) — nothing
about it is specific to the flats profiler. Arguments after the bare `--` are
passed straight through as `argv` to the app's own `main()`, exactly as they
would be on macOS.

## What this can and cannot verify on-device

**Can:** anything headless and deterministic — `--selftest`, correctness
checks like `--flats-gpu-check`, and performance of any pure function via a
`--profile-*` harness. This is the whole reason those harnesses exist in this
project as a *pattern*, not just for one feature: per
`app/ProfileVectorWarp.hpp`'s own header comment, `devicectl` can install,
launch and stream output from a real device, but **nothing in this toolchain
can synthesize a finger-drag on the physical glass** — there is no
scriptable substitute for a live touch gesture on a CoreDevice-managed iPad
the way the simulator's `mcp__Claude_Code_iOS_Simulator__control` can tap a
*simulator*. So performance or correctness questions that depend on live
touch input (stroke smoothing, pointer-to-paint latency, gesture
recognition) are NOT answerable this way — write a headless harness that
calls the same production function the touch handler would call, exactly the
shape `ProfileVectorWarp`/`ProfileToggle`/`ProfileFlatsSolver` already use,
rather than trying to script a gesture.

**Cannot:** visual/interactive verification (does it *look* right, does a
drag *feel* right) — that needs either a human with the physical device, or
the simulator (touch-as-mouse via the simulator control tool) as a
stand-in, understanding the simulator is a different device for performance
purposes and its numbers do not transfer to real hardware.

## Common mistakes to avoid

- **Confusing the simulator and device toolchains.** `iphonesimulator` SDK
  builds cannot install via `devicectl device install app` (wrong
  architecture slice for on-device use even though both are `arm64`), and a
  device build cannot boot in the simulator. Check `CMAKE_OSX_SYSROOT` in
  whatever build directory you're about to use before spending time on it.
- **Forgetting `--console`** on `process launch` and then concluding the run
  "did nothing" — it ran, you just didn't wait for or see its output.
- **Not checking `devicectl list devices` first** when a launch hangs or
  fails mysteriously — an unlocked-but-asleep device, or one that dropped its
  trust relationship, fails in ways that look like a build or code problem
  but aren't.
- **Re-running a slow cold configure** instead of borrowing an existing
  `build-ios-device/_deps` directory's fetched sources per the configure
  command above, if one already exists anywhere on the machine.
