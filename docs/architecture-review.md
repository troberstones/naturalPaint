# naturalPaint — structural and performance review

**Reviewer's stance.** Senior systems developer. Bias toward few dependencies, data-oriented
layout, and using the machine that is actually present. The lens is Casey Muratori's:
*non-pessimization* first (stop writing code in shapes that are inherently slow), *semantic
compression* second (let the abstraction fall out of the code that exists, don't erect it in
advance), and a standing refusal to accept "it's fast enough" on hardware capable of two orders
of magnitude more.

Sources for that philosophy, since it is the yardstick used throughout:
[Semantic Compression](https://caseymuratori.com/blog_0015) ·
[Complexity and Granularity](https://caseymuratori.com/blog_0016) ·
["Clean" Code, Horrible Performance](https://www.computerenhance.com/p/clean-code-horrible-performance) ·
[SE Radio 577 interview](https://se-radio.net/2023/08/se-radio-577-casey-muratori-on-clean-code-horrible-performance/)

---

## 0. Verdict in one paragraph

The **architecture** is better than most paint applications: sparse copy-on-write 128×128 tiles,
ROI-bounded ops, a dirty-tile incremental composite, bounds-checked binary parsers, and real ADRs
recording why decisions were made. The **execution against the hardware is not**. There is no
multithreading anywhere in the application (16 cores sit idle), no SIMD anywhere (128-bit NEON sits
idle), no LTO (so the hottest function in the program cannot be inlined), and the single most
expensive operation in the codebase — half↔float conversion — is a hand-written branchy software
routine on a CPU that does it in one instruction. Measured below: **the pixel loop runs 159× slower
than the same loop written to the machine.** Separately, the project has structural problems that
are not about speed: a 57,902-line test suite is compiled and linked into the shipping binary, the
binary hard-codes absolute paths into the author's source tree so it cannot be distributed at all,
and a heavyweight imaging dependency became mandatory because the native save format was built on
EXR.

---

## 1. The measurement

Everything in §2 rests on this, so it comes first. One full-canvas pass applying a trivial per-texel
op (`rgb *= 1.25`) over a 4096×4096 RGBA-f16 document — 1024 tiles, 16.8 Mtexel. Compiled `-O2`,
which is what `RelWithDebInfo` gives you. Run on the dev machine: **Apple M4 Max, 12 P-cores + 4
E-cores**.

| | approach | time | throughput |
|---|---|---:|---:|
| **A** | `Tile::readPixel`/`writePixel` as shipped — out-of-line scalar `halfToFloat`/`floatToHalf`, one texel at a time | **136.5 ms** | 0.12 Gtexel/s |
| **B** | identical loop, `_Float16` hardware convert | **7.5 ms** | 2.23 Gtexel/s |
| **C** | NEON, 8 halves per instruction | **2.2 ms** | 7.48 Gtexel/s |
| **D** | C + `dispatch_apply` across tiles | **0.86 ms** | 19.57 Gtexel/s |

**B is 18× A. D is 159× A.**

Read those rows carefully, because they say different things:

- **A→B (18×) costs nothing but deleting code.** No vectorization, no threading, no restructuring —
  the same scalar loop, with `src/core/Half.cpp`'s 40 lines replaced by a cast. That 18× is pure
  pessimization being removed.
- **B→C (3×)** is the SIMD width the compiler could not use, because the conversion was a call into
  another translation unit.
- **C→D (2.5×)** is the other 15 cores.

At row D the loop is moving ~310 GB/s and has become memory-bandwidth-bound, which is exactly where
a pixel loop should end up. Row A is bound on nothing but its own overhead.

> Caveat, stated honestly: the op is trivial, so A's overhead dominates the ratio. A heavier filter
> narrows it. But the ~129 ms of *pure conversion and call overhead* is a constant that every filter,
> every composite, every export and every save pays on top of its real work — it does not shrink.

---

## 2. Findings

Ordered by (impact × how cheaply it can be fixed). Each has the concrete change, not just the
diagnosis.

### P0-1 — `core/Half.cpp` is a software emulation of an instruction the CPU has

**File:** [`src/core/Half.cpp`](src/core/Half.cpp), [`src/core/Half.hpp`](src/core/Half.hpp:31)

`halfToFloat` and `floatToHalf` are hand-rolled bit manipulation with branches and — in the denormal
path — a `while` loop. They are **out-of-line, in their own translation unit**, called **four times
per pixel per read and four per write**. There are **264** `readPixel`/`writePixel` call sites and
**117** direct conversion call sites.

Every target this project can run on does this in hardware: AArch64 has `fcvt` (and `vcvt_f32_f16`
for 4-at-a-time); x86-64 has had F16C `vcvtph2ps` since 2013.

**Fix — the whole change:**

```cpp
// core/Half.hpp -- header-only, no .cpp
#pragma once
#include <cstdint>
#include <bit>
namespace np {
#if defined(__ARM_FP16_FORMAT_IEEE) || defined(__F16C__)
inline float halfToFloat(uint16_t h) noexcept { return static_cast<float>(std::bit_cast<_Float16>(h)); }
inline uint16_t floatToHalf(float f) noexcept { return std::bit_cast<uint16_t>(static_cast<_Float16>(f)); }
#else
// keep the existing software path here as the portable fallback
#endif
}
```

Then **delete `src/core/Half.cpp`** and its line in `src/CMakeLists.txt`.

Two things this buys beyond the 18×: the conversions become inlinable, which is the precondition
for the compiler autovectorizing any of the 264 call sites; and `_Float16` uses the same IEEE-754
binary16 rounding the software path was hand-implementing, so results are bit-identical for
everything except the NaN payload — which the selftest suite will confirm.

**Effort:** one afternoon including verifying the suite still passes. **This is the single highest
return-on-effort change in the repository.**

---

### P0-2 — No LTO, so cross-TU hot functions are real calls forever

**File:** [`src/CMakeLists.txt`](src/CMakeLists.txt)

The per-pixel inner loop of `core/Composite.cpp` calls:

- `Tile::readPixel` → 4× `np::halfToFloat` — **in `Half.cpp`, another TU**
- `blendPixel(mode, src, dst)` — **in `Blend.cpp`, another TU**, with a switch over ~27 modes inside
- for adjustment layers, `applyPointOpsPremultiplied` — **in `PointOps.cpp`, another TU**

None of these can be inlined. The compiler cannot hoist the blend-mode switch out of the loop (it is
a per-layer constant), cannot vectorize across texels, cannot keep anything in registers across the
call boundary. For a 4096² document with 10 full-canvas layers that is 5 calls per texel per layer
— **roughly 840 million non-inlinable calls per full composite**.

**Fix:**

```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok)
if(ipo_ok)
  set_property(TARGET naturalPaint PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
```

Fixing P0-1 makes `Half` header-only and removes the worst of it without LTO; LTO catches
`blendPixel` and `applyPointOpsPremultiplied` too.

**Effort:** 5 lines. Expect a longer link and a meaningfully faster binary.

---

### P0-3 — Zero multithreading in an application built on an embarrassingly parallel data structure

**Evidence:** one `std::thread` in the entire tree, and it is in
[`src/app/selftest/FileDialog.cpp:217`](src/app/selftest/FileDialog.cpp:217). Application code: none.

This is the finding that should be most uncomfortable, because the architecture was *already
designed for it and then not used*. The document is a sparse map of independent 128 KiB tiles. Every
filter in `ops/` walks tiles in a nested loop where **iterations touch disjoint memory**. Blur,
resample, transform, feather, the composite, the f16 pack for upload — all of them are a
`parallel_for` over `TileCoord` with no synchronization required beyond the existing store.

The machine has 16 cores. The app uses 1.

**Fix.** Do not add a dependency for this. On macOS, `dispatch_apply` is in libSystem, already
linked, and is what row D above used:

```cpp
// core/Parallel.hpp -- the entire threading layer
#pragma once
#include <cstddef>
#include <dispatch/dispatch.h>
namespace np {
// Serial below `grain` so small ROIs don't pay dispatch overhead; the crossover
// is worth measuring once rather than guessing (start around 8 tiles).
template <class F>
void parallelFor(size_t n, size_t grain, F&& body) {
  if (n < grain) { for (size_t i = 0; i < n; ++i) body(i); return; }
  __block F& f = body;
  dispatch_apply(n, DISPATCH_APPLY_AUTO, ^(size_t i) { f(i); });
}
}
```

Then in each op, flatten the tile loop into an index and hand it to `parallelFor`. Concretely, in
[`ops/Filters.cpp`](src/ops/Filters.cpp:107)'s `scatterAligned`, the `ty`/`tx` pair becomes one
linear tile index; the body is unchanged.

**One ordering constraint to respect:** `TileStoreOf::getOrCreate` mutates the `unordered_map` and is
**not** safe to call concurrently. So the pattern is two phases — serially reserve every destination
tile the ROI touches, then parallelize the pixel work over the now-stable slots. That is a small
refactor per op and it is the only thread-safety work the whole change needs.

**Effort:** ~1 day for the helper plus the first three ops (blur, filters, composite). Roughly 2.5×
on top of everything else.

---

### P0-4 — The full composite is layer-major over a 256 MB accumulator

**File:** [`src/core/Composite.cpp:445`](src/core/Composite.cpp:445), [`:850`](src/core/Composite.cpp:850)

`compositeDocumentPremultiplied` allocates `std::vector<float>` of `width * height * 4` — **268 MB
for a 4096² document, zero-filled on every call** — then loops **layers on the outside**, scattering
each layer's tiles into it.

For an N-layer document that is N full passes over a 268 MB buffer. Nothing stays in cache between
layers; every layer re-streams the whole accumulator from DRAM. A tile that was hot in L2 for layer 3
is long evicted by the time layer 4 wants it.

**Invert the loops.** Tile on the outside, layers on the inside:

```
for each tile coord in the document:          // parallelizable, disjoint
    float accum[128*128*4];                   // 256 KiB, stays in L2
    for each layer:                           // now the inner loop
        if layer has this tile: blend into accum
    write accum out / convert to f16
```

Total traffic drops from `N × 268 MB` to roughly `268 MB` once, the accumulator becomes a stack
buffer instead of a heap allocation, and the outer loop is *exactly* the `parallelFor` from P0-3.

**Scope note, in fairness:** the interactive path already avoids this. `ui/DocumentTexture.cpp`'s
dirty-tile band walk keeps the working set small during painting. The layer-major cost lands on
**first open, export, flatten, and save** — the operations where the user is watching a spinner. It
is worth fixing, but it is not costing you frame rate while painting.

**Secondary, same file:** `compositeDocumentStraightHalf`
([`ui/DocumentTexture.cpp:24`](src/ui/DocumentTexture.cpp:24)) takes the 268 MB float result and
allocates a *second* full-document buffer to convert it to f16. Fuse the unpremultiply-and-pack into
the tile loop above and the second allocation disappears entirely.

---

### P0-5 — `std::function` called per pixel, when the fast path already exists in this repo

**Files:** [`src/ops/PointOps.hpp:263`](src/ops/PointOps.hpp:263),
[`src/core/Composite.cpp:499`](src/core/Composite.cpp:499)

```cpp
using PointOp = std::function<std::array<float, 3>(const std::array<float, 3>&)>;
```

For every pixel under an adjustment layer, the compositor calls `adjustedPremultiplied`, which calls
`applyPointOpsPremultiplied`, which **iterates a `std::vector<std::function<…>>` making one indirect
call per op per pixel**. Type erasure defeats inlining, defeats vectorization, and every call is an
indirect branch the predictor has to learn.

This is the textbook case from ["Clean" Code, Horrible
Performance](https://www.computerenhance.com/p/clean-code-horrible-performance) — polymorphism
applied at the granularity of a single pixel, for a pipeline that is **constant for the entire
layer**.

**The infuriating part: the correct implementation is already in the tree and is not used.**
[`docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md`](docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md)
decided this. [`src/color/LutBake.cpp`](src/color/LutBake.cpp) implements it. `shaders/lut_op_*.wgsl`
run it. And its **only consumer is a self-test** — `runSelfTest`'s `runLutBakeTest`. The CPU
compositor, the one that runs on every save and export, still walks the `std::function` chain.

**Fix:** bake the layer's op stack into the existing `Lut3D` (`kLutSize = 32`) once per stack
revision — `core/OpStack` already carries a version for exactly this — and replace the per-pixel
call chain with a tetrahedral lookup. `OpStack.hpp:161` says the version exists "so color/LutBake can
compare across frames to decide whether to rebake." Wire it up.

Interim, if the LUT is too big a step: replace `std::vector<std::function>` with a small tagged
struct plus a `switch`. It is six ops (`Levels`, `Curves`, `Exposure`, `Saturation`, `Grayscale`,
`ChannelMixer`), a closed set, and `-Werror=switch` is already on to keep it honest.

---

### P1-1 — 57,902 lines of tests are compiled and linked into the shipping binary

**File:** [`src/CMakeLists.txt`](src/CMakeLists.txt) — 115 `app/selftest/*.cpp` entries in
`add_executable(naturalPaint …)`

| | files | lines | share |
|---|---:|---:|---:|
| self-test suite | 117 | 57,902 | **37.8%** |
| everything else | 235 | 95,407 | 62.2% |

The whole project is **one `add_executable`**. No libraries, no test target. Consequences:

1. Every user gets a binary 37% of which is test code, plus all its string literals and fixtures.
2. Nobody can build or run a test without SDL3, ImGui, wgpu-native, a GPU, **and** OpenImageIO.
   Testing `floorDiv` requires a working Metal device.
3. Touching any header rebuilds the tests along with the app.
4. Test-only code sits in the same address space as user documents.

**Fix — this is also the fix for build time:**

```cmake
add_library(np_core STATIC core/… color/… ops/… io/… brush/…)   # no SDL, no GPU
add_library(np_app  STATIC app/… ui/… sim/… gfx/…)
add_executable(naturalPaint main.cpp)          # links np_app np_core
add_executable(np_selftest  app/selftest/…)    # links np_core (+np_app only where needed)
```

Most of `core/`, `ops/`, `color/` and `io/` genuinely has no GPU dependency. Split the suite along
that line: the pure-CPU majority becomes a fast headless test target, and only the handful of tests
that need a device link the app library. `--selftest` can stay as a shipping smoke test if you want
it, but it should be a thin runtime check, not 57k lines.

---

### P1-2 — The binary cannot leave the machine that built it

**File:** [`src/CMakeLists.txt`](src/CMakeLists.txt) `target_compile_definitions`

```cmake
NP_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
NP_MIXBOX_LUT="${CMAKE_SOURCE_DIR}/third_party/mixbox/shaders/mixbox_lut.png"
NP_KEYMAP_DIR="${CMAKE_SOURCE_DIR}/keymaps"
NP_LUCIDE_TTF="${CMAKE_SOURCE_DIR}/third_party/lucide/lucide.ttf"
```

Four **absolute paths into the author's home directory**, baked into the executable. There is no
`install()` rule, no bundle, no `CPack`. Copy the binary anywhere and it loses its shaders, its
pigment LUT, its keymap and its icon font.

Worse, the failure modes differ. `ui/Fonts.cpp:332` checks for the font and **silently degrades to
hand-drawn vectors** — so on any machine but this one, the toolbar quietly renders differently and
nothing says why. `main.cpp:1311` on the Mixbox LUT is fatal.

The comment says these are source-tree paths so shaders can be hot-reloaded with Cmd+R. That is a
good reason — but it argues for a *development override*, not for it being the only mode.

**Fix:** resolve assets relative to the executable at runtime, with the compiled-in source path as a
**debug-build-only** fallback and an `NP_ASSET_DIR` environment override for hot-reload. Add an
`install()` rule and a `.app` bundle target. Hot reload keeps working; the binary becomes shippable.

---

### P1-3 — OpenImageIO became mandatory because the native format was built on EXR

**File:** [`CMakeLists.txt`](CMakeLists.txt:36-80) — a 45-line comment arguing the reversal

The reasoning in that comment is honest and internally correct: *"A foreign format you cannot read is
a capability gap. A document you cannot save is not an application."* Given `io/NpaintFile` builds
`.npaint` on multi-part EXR, making OIIO optional made saving optional, which was untenable. Right
call **on the question asked**.

But it is the wrong question. The right one is: *why does the native format depend on the heaviest
imaging library in the ecosystem?* OpenImageIO drags in OpenEXR, Imath, libtiff, libjpeg, libpng,
zlib, fmt and more — and this project already had to hand-build a stripped copy to `~/.local` with
LibRaw, ffmpeg and Python cut out, which `find_package` then cannot locate without a manual
`CMAKE_PREFIX_PATH`. For a project whose stated value is *few dependencies*, the native save path is
the single largest transitive tree in the build, and every contributor pays a bespoke OIIO build
before they can compile at all.

**Fix.** The document is already a sparse map of fixed-size f16 tiles — the container practically
writes itself: a header, a tile directory, and the raw tile bytes, optionally deflated with
`stbi_zlib_compress()` — already compiled into the binary and already used in production by
`io/Export.cpp`'s `encodePng16()`, so compression costs no new dependency. A few hundred lines, and
it round-trips the actual in-memory structure instead of forcing it through a format designed for
something else.

OIIO then returns to what the original argument correctly described it as: an **optional** backend
for foreign formats (EXR/TIFF/HDR/DPX/PSD), queried at runtime, exactly as PRD I3 specified.

**Effort:** 2–3 days. It removes the largest dependency in the project from the critical path and
makes a from-scratch build a `cmake && make`.

---

### P1-4 — The GPU is present, initialized, and does none of the image work

31 WGSL shaders exist. Every one serves the fluid/pigment simulation or the grade preview. Grep for
blur, resample, transform, gradient, feather, flood-fill shaders: **none**.

So the app maintains a full WebGPU context, uploads the document to a texture every frame, and then
does every filter and every geometric transform on **one CPU core, scalar**, converting f16→f32→f16
in software on the way — while the GPU that already holds the pixels idles.

**Recommendation, deliberately narrow.** Do not port everything; that is how a codebase acquires two
divergent implementations of twelve operations. Port the ones where the CPU is worst and the shader
is trivial:

| op | why | shader |
|---|---|---|
| Gaussian blur | separable, huge kernel cost on CPU, feeds unsharp/highpass/local-contrast | ~30 lines, two passes |
| Resample | bilinear/bicubic is what texture units *are* | ~20 lines |
| Transform | ditto, plus it's already a matrix | ~20 lines |

That is three shaders covering the operations that dominate. Everything else stays on the CPU where,
after P0-1 through P0-3, it will be roughly 100× faster than today and entirely adequate.

**Sequencing note:** do §P0-1..3 *first*. A large fraction of what currently looks like "needs the
GPU" is just the software half-float path, and you should not port an op to WGSL to escape a problem
you can delete in an afternoon.

---

### P2-1 — Three dependent loads to reach a pixel

**File:** [`src/core/TileStore.hpp:328`](src/core/TileStore.hpp:328)

```cpp
using Slot = std::shared_ptr<T>;
using Map  = std::unordered_map<TileCoord, Slot>;
```

Reaching one texel is: hash → bucket array → **node** (`unordered_map` is node-based; separate
allocation, pointer chase) → **shared_ptr control block** → tile data. Three dependent loads, each a
potential cache miss, before the pixel. Plus an atomic refcount on every copy.

The `shared_ptr` is load-bearing — it *is* the copy-on-write sharing primitive, and the header argues
that well. Keep the semantics. Change the container:

**Fix:** an open-addressing map with the `TileCoord` keys and slot pointers in **flat parallel
arrays** (the splitmix64 hash in `Tile.hpp` is already good). This removes the node chase and makes
iteration — which every op does — linear instead of a pointer walk. `TileStoreOf` is already a
template with a narrow interface (`find`, `getOrCreate`, iterate), so this is contained behind that
interface; no caller changes.

**Effort:** ~200 lines, well covered by the existing `TileStore`/`CowTile` tests. Do it after
P0-1..4 — it is a real win but a smaller one, and it is the kind of change that wants a green suite
underneath it.

**Related, smaller:** `sizeof(Tile) == 131072` and tiles are individually `make_shared`'d. A pool
allocator handing out 128 KiB blocks from a slab would cut allocator pressure and improve locality
between neighbouring tiles. Worth doing once the map is flat.

---

### P2-2 — Security posture

The parsers are **better than expected** and deserve saying so: `io/AbrBrushes.cpp` uses
`std::span`, bounds-checked `readU16`/`readU32` returning `bool`, refuses inverted rectangles
explicitly to avoid u32 underflow, and caps sampled-tip dimensions. Someone was thinking about
hostile input. Findings are hardening, not holes:

1. **`size_t` overflow idiom.** `if (at + 2 > b.size())`
   ([`AbrBrushes.cpp:19`](src/io/AbrBrushes.cpp:19)), `if (hoff + 19 <= bodyEnd)`, `if (p + count >
   dataEnd)`. The addition can wrap before the compare. Not reachable today — offsets are bounded by
   file size — but it is the wrong shape and one refactor from being reachable. Write it as
   `if (at > b.size() - 2)` with a `b.size() >= 2` guard, or use a checked-add helper.
2. **Ignored return values.** [`AbrBrushes.cpp:727-733`](src/io/AbrBrushes.cpp:727) calls `readU32`
   four times and `readU16` once, discarding every result. The preceding `hoff + 19 <= bodyEnd`
   guard makes them safe *provided* `bodyEnd <= samp.size()` — an invariant held two scopes away. Mark
   the readers `[[nodiscard]]` and handle it.
3. **No sanitizers, ever.** Nothing in the tree mentions `-fsanitize`. A 153k-line C++ application
   that parses ABR, PSD, EXR, PNG and JPEG from untrusted files should have an ASan+UBSan build
   configuration and should run the self-test suite under it in CI.
4. **No fuzzing.** The parsers are the highest-value fuzz targets imaginable and are already shaped
   for it — `decodePackBits` and `parseAbr` take a span and return bool. A libFuzzer harness is ~15
   lines each. This is the highest security return in the document.
5. **Unverified binary dependency.** [`cmake/Dependencies.cmake`](cmake/Dependencies.cmake:43) tells
   the user to `curl -L … | unzip` a 13 MB prebuilt `libwgpu_native.a` **with no checksum**. Pin the
   SHA-256 and verify it in CMake. As written, anyone who can MITM that download or compromise the
   release asset gets arbitrary code into every build.
6. **No hardening flags.** Add `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` (bounds-checks
   `std::array`/`std::vector` indexing at near-zero cost) and `-fstack-protector-strong`.

---

### P2-3 — The Mixbox licence has no working escape hatch

`NP_USE_MIXBOX` is documented in [`CMakeLists.txt:32`](CMakeLists.txt:32) as the switch that falls
back to "a plain Kubelka-Munk two-constant mix with no licence encumbrance." It defaults `ON`, and
`src/CMakeLists.txt:334` defines it.

**No source file reads it.** Three matches in the whole tree: the two CMake lines that define it, and
a comment in `SelfTest.hpp` observing that it had rotted. There is no `#if defined(NP_USE_MIXBOX)`
anywhere. Turning it off changes nothing.

Mixbox is **CC BY-NC**. Non-commercial only. So the documented path to a distributable build does not
exist, and the project is licence-locked to non-commercial use while its own build system claims
otherwise. `docs/adr/0006-mixbox-now-reimplement-from-the-paper-later.md` planned for this; the plan
was not executed and the flag makes it look as though it was.

**Fix:** either implement the fallback behind the flag and build both configurations in CI (an
unbuilt configuration is not a seam — `PLAN.md` 1.5 already says so), or delete the option and state
the licence constraint plainly in the README. The current state is the worst of the three.

---

### P3 — Readability and navigability

Not performance, but they are why the items above are expensive to fix.

**Comments are 51% of the non-test source.**

| | lines |
|---|---:|
| non-test source | 95,407 |
| comment lines | **49,068 (51%)** |
| blank | 7,022 |
| **actual code** | **39,317** |

The quality is genuinely high — these are design rationale, not restatements of the code, and the
`Half.cpp`/`TileStore.hpp`/`CMakeLists.txt` comments each explain a real decision. But at 1.25 lines
of prose per line of code, the prose becomes a **second artifact that has to be maintained in sync
with the first**, and this project already has documented cases where it drifted: the `NP_USE_MIXBOX`
comment above describes a fallback that does not exist, and `CMakeLists.txt` itself notes ~230
`#if defined(NP_USE_OIIO)` sites whose `#else` branches are now unreachable and whose comments
therefore mislead.

Recommendation: move the *why* — the multi-paragraph arguments — into `docs/adr/`, which already
exists and is the right home for them. Leave short pointers at the code. A 45-line argument inside
`CMakeLists.txt` is an ADR that lost its way.

**Two files should be split:**
- [`src/ui/MacPaintUI.cpp`](src/ui/MacPaintUI.cpp) — **10,886 lines**. This is where the split of
  `SelfTest.cpp` (noted in `src/CMakeLists.txt` as having been forced by merge conflicts) is heading
  next.
- [`src/main.cpp`](src/main.cpp) — 3,244 lines, of which `main()` is **2,253** (lines 991–3244),
  interleaving argument parsing, GPU init, demo builders, screenshot verification and the event loop.

Split both by *operation*, not by type — which, per
[Semantic Compression](https://caseymuratori.com/blog_0015), is also the arrangement that makes the
duplication visible enough to compress.

---

## 3. Sequence

Ordered so each step makes the next cheaper, and so nothing is optimized before the thing under it is
fixed.

**Week 1 — remove pessimization. No architecture changes.**
1. P0-1: `Half` → header-only `_Float16`. Delete `Half.cpp`. *(~18× on every pixel path)*
2. P0-2: enable IPO/LTO.
3. Re-run `--selftest` and `tools/golden/run_golden.sh`. Both should pass unchanged — this is a
   representation change, not a semantic one.

**Week 2 — use the cores.**
4. P0-3: `core/Parallel.hpp` with `dispatch_apply`. Reserve-then-parallelize in `getOrCreate` callers.
5. Apply it to `ops/Filters.cpp`, `ops/Blur.cpp`, `core/Composite.cpp`. *(~2.5×)*

**Week 3 — fix the loop shapes.**
6. P0-4: invert composite to tile-major; fuse the f16 pack; drop the second full-document allocation.
7. P0-5: bake the op stack to `Lut3D` on the CPU path. The code exists — connect it.

**Week 4 — structure.**
8. P1-1: split into `np_core` / `np_app` / `naturalPaint` / `np_selftest`.
9. P2-2 items 3 and 4: ASan/UBSan build, libFuzzer harnesses for `parseAbr` and `decodePackBits`,
   both in CI.

**Then, as separate pieces of work:**
10. P1-2: runtime asset resolution + `install()` + bundle.
11. P1-3: native `.npaint` container; OIIO back to optional.
12. P1-4: three GPU shaders (blur, resample, transform) — *only after re-measuring*.
13. P2-1: flat open-addressing `TileStore`.
14. P2-3: Mixbox fallback, or an honest README.

---

## 4. What not to do

- **Do not add a threading library.** `dispatch_apply` is in libSystem and is already linked. TBB,
  OpenMP and `std::execution` all buy less than the 12 lines in P0-3.
- **Do not add a SIMD wrapper library.** After P0-1 the compiler autovectorizes most of these loops.
  Hand-write NEON only where a profile says it matters, and only behind the existing tile interface.
- **Do not port everything to the GPU.** Two implementations of twelve ops is worse than one good CPU
  implementation of twelve. Three shaders, chosen by measurement.
- **Do not restructure before Week 1 lands.** The 18× is sitting in one 40-line file. Everything else
  is a smaller multiplier on a larger diff.
- **Do not treat the 51% comment ratio as something to strip.** The reasoning is good and worth
  keeping. Relocate it to `docs/adr/`; do not delete it.

---

## 5. Credit where it is due

Reviews skew negative; this one should not be read as "the project is bad." These are genuinely
well-made decisions and they are why the fixes above are tractable at all:

- **Sparse copy-on-write 128×128 f16 tiles** with a `static_assert` on `sizeof(Tile)`. The right
  primitive. Every parallelization opportunity in this document exists *because* of this choice.
- **`floorDiv`/`floorMod` with the sign reasoning written down.** The truncation bug this avoids is
  one almost every canvas application ships at least once.
- **ROI discipline.** `ops/Roi.hpp` and the `outRect` parameter threaded through every filter mean
  ops already do bounded work — the thing most paint applications retrofit painfully.
- **The dirty-tile incremental composite** in `ui/DocumentTexture.cpp`. This is why the app is
  responsive while painting despite everything in §2.
- **`-Werror=switch`, `-Werror=unused-function`** — and the note recording that a subagent *tested
  that the flag actually bites* rather than assuming it. That instinct is the one that matters.
- **Bounds-checked, span-based binary parsers** that refuse rather than wrap, with the u32-underflow
  case called out by name.
- **ADRs that record why**, including the ones that later turned out to be wrong. That is what makes
  P1-3 a two-day change instead of an archaeology project.

The architecture is sound. What it needs is for the innermost loops to stop apologizing to hardware
that stopped being slow twenty years ago.
