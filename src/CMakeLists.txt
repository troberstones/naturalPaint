add_executable(naturalPaint
  main.cpp
  app/BrushLibraryFile.cpp
  app/BrushRowIcon.cpp
  app/CloseDecision.cpp
  app/CompPanel.cpp
  app/ControlsLayout.cpp
  app/CurveEdit.cpp
  app/DabPreview.cpp
  app/DocumentLifecycle.cpp
  app/ImportImage.cpp
  app/Journal.cpp
  app/HistoryPanel.cpp
  app/Keymap.cpp
  app/PenAxes.cpp
  app/Screenshot.cpp
  app/LayerEditor.cpp
  app/LayerPanel.cpp
  app/Latency.cpp
  app/Memory.cpp
  # One translation unit per --selftest section, plus Driver.cpp for the three
  # drivers and Support.cpp for what they share (src/app/selftest/Support.hpp).
  # These were one 20 815-line app/SelfTest.cpp until every parallel branch
  # started conflicting in it; app/SelfTest.hpp is still the single index of
  # what the suite proves and is deliberately NOT split. Listed explicitly
  # rather than globbed, same as every other source in this target, so adding
  # a section is a visible line in the diff and CMake reconfigures for it.
  app/selftest/Accumulator.cpp
  app/selftest/AdjustmentLayer.cpp
  app/selftest/ApplyPass.cpp
  app/selftest/BaseLayerAlpha.cpp
  app/selftest/AbrBrushes.cpp
  app/selftest/Blend.cpp
  app/selftest/Blur.cpp
  app/selftest/BrushDynamics.cpp
  app/selftest/BrushLibraryFile.cpp
  app/selftest/BucketRefusal.cpp
  app/selftest/Channels.cpp
  app/selftest/Clipboard.cpp
  app/selftest/ClippingMask.cpp
  app/selftest/CloseDecision.cpp
  app/selftest/ColorSpace.cpp
  app/selftest/ControlsLayout.cpp
  app/selftest/CowTile.cpp
  app/selftest/CreateBlank.cpp
  app/selftest/CurveEdit.cpp
  app/selftest/DabPreview.cpp
  app/selftest/Descriptor.cpp
  app/selftest/Document.cpp
  app/selftest/DocumentLifecycle.cpp
  app/selftest/DocumentResidency.cpp
  app/selftest/DocumentTexture.cpp
  app/selftest/DocumentTransform.cpp
  app/selftest/Driver.cpp
  app/selftest/Export.cpp
  app/selftest/ExportAs.cpp
  app/selftest/ExportStates.cpp
  app/selftest/Eyedropper.cpp
  app/selftest/FieldAllocation.cpp
  app/selftest/FilterMenu.cpp
  app/selftest/Filters.cpp
  app/selftest/FloodFill.cpp
  app/selftest/Fonts.cpp
  app/selftest/AtelierChrome.cpp
  app/selftest/ActiveLayer.cpp
  app/selftest/FormatSupport.cpp
  app/selftest/Gradient.cpp
  app/selftest/GuidesGridSnap.cpp
  app/selftest/Histogram.cpp
  app/selftest/History.cpp
  app/selftest/HistoryPanel.cpp
  app/selftest/IdleMemory.cpp
  app/selftest/ImageDecode.cpp
  app/selftest/ImageIO.cpp
  app/selftest/IncrementalComposite.cpp
  app/selftest/Keymap.cpp
  app/selftest/LayerComp.cpp
  app/selftest/LayerEditor.cpp
  app/selftest/LayerMask.cpp
  app/selftest/LayerMultiSelect.cpp
  app/selftest/LayerPanel2a.cpp
  app/selftest/LayerStack.cpp
  app/selftest/LutBake.cpp
  app/selftest/MenuBasics.cpp
  app/selftest/MenuModel.cpp
  app/selftest/MergeFamily.cpp
  app/selftest/MipPyramid.cpp
  app/selftest/NpaintFormat.cpp
  app/selftest/OpenAnyFile.cpp
  app/selftest/OpStack.cpp
  app/selftest/PigmentBake.cpp
  app/selftest/PigmentBasis.cpp
  app/selftest/PigmentDeposit.cpp
  app/selftest/PigmentLayer.cpp
  app/selftest/PigmentSelection.cpp
  app/selftest/PlaceImageAsLayer.cpp
  app/selftest/PointOps.cpp
  app/selftest/PresentTransfer.cpp
  app/selftest/Probe.cpp
  app/selftest/QuitGuard.cpp
  app/selftest/RecoveryJournal.cpp
  app/selftest/RgbDeposit.cpp
  app/selftest/RgbErase.cpp
  app/selftest/Selection.cpp
  app/selftest/SelectionBoundary.cpp
  app/selftest/SelectionRefine.cpp
  app/selftest/SelectionShapes.cpp
  app/selftest/SelectionTools.cpp
  app/selftest/Shaper.cpp
  app/selftest/StrokeBridge.cpp
  app/selftest/StrokeSpeed.cpp
  app/selftest/Support.cpp
  app/selftest/TileResidency.cpp
  app/selftest/TileStore.cpp
  app/selftest/ToolCursor.cpp
  app/selftest/Transform.cpp
  app/selftest/ViewTransform.cpp
  app/FilterOps.cpp
  app/OpenAnyFile.cpp
  app/QuitSequence.cpp
  app/Snapping.cpp
  app/StrokeBake.cpp
  app/StrokeSession.cpp
  app/ViewTransform.cpp
  brush/Deposit.cpp
  brush/Dynamics.cpp
  brush/Library.cpp
  brush/PigmentErase.cpp
  brush/RgbDeposit.cpp
  brush/RgbErase.cpp
  brush/StrokePath.cpp
  color/LutBake.cpp
  color/Shaper.cpp
  color/Space.cpp
  core/Blend.cpp
  core/Channels.cpp
  core/Clipboard.cpp
  core/Composite.cpp
  core/DirtyTiles.cpp
  core/Half.cpp
  core/Histogram.cpp
  core/History.cpp
  core/LayerCompOps.cpp
  core/LayerGeometry.cpp
  core/LayerOps.cpp
  core/LayerSetOps.cpp
  core/Merge.cpp
  core/OpStack.cpp
  core/Pigment.cpp
  core/PigmentBake.cpp
  core/Probe.cpp
  core/SelectionBoundary.cpp
  core/SelectionMask.cpp
  core/SelectionOps.cpp
  core/SelectionRefine.cpp
  core/SelectionShapes.cpp
  gfx/Context.cpp
  gfx/ShaderLoader.cpp
  io/AbrBrushes.cpp
  io/Capabilities.cpp
  io/CompSerial.cpp
  io/Descriptor.cpp
  io/Export.cpp
  io/ExportAs.cpp
  io/ExportStates.cpp
  io/FileKind.cpp
  io/ImageDecode.cpp
  io/ImageIO.cpp
  io/NpaintFile.cpp
  io/OpSerial.cpp
  io/TileResidency.cpp
  ops/Blur.cpp
  ops/DocumentTransform.cpp
  ops/Feather.cpp
  ops/Filters.cpp
  ops/FloodFill.cpp
  ops/Gradient.cpp
  ops/PointOps.cpp
  ops/Resample.cpp
  ops/Roi.cpp
  ops/Transform.cpp
  paint/Palette.cpp
  sim/PaintSim.cpp
  ui/DocumentTexture.cpp
  ui/MacPaintUI.cpp
  ui/NaturalPaintUI.cpp
  ui/Fonts.cpp
  ui/AtelierTheme.cpp
  ui/CanvasQuad.cpp
  ui/AtelierLayout.cpp
  ui/AtelierChrome.cpp
  ui/MenuModel.cpp
  ui/ToolCursor.cpp
)

# ---- the native menu bar (ui/MacNativeMenu.mm) ------------------------------
#
# macOS puts an application's menus in the bar at the top of the *screen*, not
# in a strip inside the window, and a painting application that does not is
# immediately and permanently foreign. ui/MenuModel.hpp is the platform-neutral
# description both bars are built from; this is the AppKit half, and it is the
# first Objective-C++ in this project.
#
# **`enable_language(OBJCXX)` is in the ROOT CMakeLists.txt, not here**, and the
# reason is a CMake rule rather than a preference: `enable_language()` must be
# called from the highest directory common to every target that uses the
# language. Called from this subdirectory it appears to succeed -- no error is
# printed at the call -- and then the generate step fails with
# `Missing variable is: CMAKE_OBJCXX_COMPILE_OBJECT`, because the per-language
# rule variables were never populated in the scope that generates the build.
# The failure names a "may not be built correctly" CMake rather than the line
# that caused it, so it reads as a broken toolchain; it is not.
#
# What stays true is why it is not in the root `project()` call's LANGUAGES
# list: a language named there is probed on **every** platform, so a Linux or
# Windows configure would go looking for an Objective-C++ compiler it has no
# reason to own, and fail at configure time over a file it was never going to
# build. So the root guards the same `if(APPLE)` around it -- right scope, and
# still only the machines that can answer are asked.
#
# `-fobjc-arc` on this one source. The alternative is manual retain/release
# around NSMenu and NSMenuItem objects that live for the process's whole
# lifetime and are rebuilt whenever a document opens or closes -- which is to
# say, a slow leak nobody would notice and a double-release everybody would.
# ARC is per-translation-unit, costs the rest of the target nothing, and the
# file is written for it. (It compiles clean both with and without, so a
# toolchain that ignored this property would still build.)
#
# Cocoa is named explicitly even though SDL3-static already pulls it in on this
# platform. This translation unit `#import`s <Cocoa/Cocoa.h> and uses NSMenu
# directly, so it has a dependency of its own; inheriting it from SDL would
# work right up until somebody made SDL a dynamic dependency.
#
# The -Werror=unused-* guard at the bottom of this file applies to this source
# too, which is intended: an Objective-C++ helper nobody calls is the same
# defect as a C++ one.
if(APPLE)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  target_sources(naturalPaint PRIVATE ui/MacNativeMenu.mm)
  set_source_files_properties(ui/MacNativeMenu.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(naturalPaint PRIVATE "-framework Cocoa")
  message(STATUS "naturalPaint: native AppKit menu bar (ui/MacNativeMenu.mm)")
endif()

target_include_directories(naturalPaint PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
# third_party is SYSTEM so the -Werror=unused-* guard at the bottom of this
# file judges only code this project owns: stb_image.h's implementation emits
# static functions a given translation unit may not call, and that is the
# vendored header's business, not a defect in src/.
target_include_directories(naturalPaint SYSTEM PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(naturalPaint PRIVATE
  SDL3::SDL3-static
  wgpu_native
  imgui
)

# Shaders, the pigment LUT and the keymap are read from the source tree at
# runtime, so the solver can be edited and reloaded (Cmd+R) and the keymap
# hand-edited, without a rebuild -- same convention for all three.
target_compile_definitions(naturalPaint PRIVATE
  NP_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
  NP_MIXBOX_LUT="${CMAKE_SOURCE_DIR}/third_party/mixbox/shaders/mixbox_lut.png"
  NP_KEYMAP_DIR="${CMAKE_SOURCE_DIR}/keymaps"
  # ui/Fonts.cpp's installToolIconFont() -- the tool palette's Lucide icons
  # (docs/ui.md section 2). Same convention as the three paths above: a
  # vendored/source-tree asset read at runtime rather than compiled in, so
  # third_party/lucide/lucide.ttf can be swapped without a rebuild.
  NP_LUCIDE_TTF="${CMAKE_SOURCE_DIR}/third_party/lucide/lucide.ttf"
  # app/selftest/AtelierChrome.cpp's own icon-name-to-codepoint verification
  # -- "every name you use MUST exist in codepoints.json" checked against the
  # vendored file itself at test time, not merely at authoring time.
  NP_LUCIDE_CODEPOINTS_JSON="${CMAKE_SOURCE_DIR}/third_party/lucide/codepoints.json"
)

if(NP_USE_MIXBOX)
  target_compile_definitions(naturalPaint PRIVATE NP_USE_MIXBOX=1)
endif()

# PLAN.md phase 4 step 2/3, amended: OpenImageIO is **required**, not optional.
# The root CMakeLists.txt carries the argument for the reversal; the short form
# is that the same guard which made *foreign* formats optional also made the
# *native* .npaint format and the recovery journal refuse, and a build that
# cannot save a document is not a configuration worth offering.
#
# io/OiioBackend.cpp remains the *only* translation unit that includes an
# OpenImageIO header. That is still worth preserving now that the dependency is
# unconditional: it keeps every other file's knowledge of the backend down to
# io/OiioBackend.hpp, which is what made this reversal a CMake change rather
# than a source-wide one.
#
# NP_USE_OIIO is still defined, unconditionally, so the ~230 `#if
# defined(NP_USE_OIIO)` sites across src/ keep telling the truth. Removing
# those guards and their now-dead `#else` branches is a separate change.
#
# Both OpenImageIO::OpenImageIO and OpenImageIO::OpenImageIO_Util are linked
# explicitly. The config package's imported target does chain the Util
# library, but naming it here is deliberate rather than redundant: ustring's
# symbols (e.g. ustring::empty_std_string) live in the Util library only, and
# an OpenImageIO install whose imported target did not chain it would fail at
# link time with an error that points nowhere near the cause.
find_package(OpenImageIO REQUIRED)
target_sources(naturalPaint PRIVATE io/OiioBackend.cpp)
target_compile_definitions(naturalPaint PRIVATE NP_USE_OIIO=1)
target_link_libraries(naturalPaint PRIVATE
  OpenImageIO::OpenImageIO
  OpenImageIO::OpenImageIO_Util
)
message(STATUS "naturalPaint: OpenImageIO ${OpenImageIO_VERSION} (required)")
# On the deployment-target link warning, which people hit when they build
# a probe against this OpenImageIO by hand and then wonder whether the
# CMake build has the same problem:
#
#   ld: warning: building for macOS-15.0, but linking with dylib
#       '@rpath/libOpenImageIO.3.1.dylib' which was built for newer
#       version 15.6
#
# **It does not occur in this build, and nothing here suppresses it.**
# Checked rather than assumed: the OpenImageIO dylibs were compiled for
# macOS 15.6, and CMake defaults CMAKE_OSX_DEPLOYMENT_TARGET to the host
# OS version -- 15.6 on this machine -- so the target's own compile line
# carries -mmacosx-version-min=15.6 and the two agree. The warning shows up
# only when the clang driver is invoked directly with no CMake and falls
# back to its own default of 15.0.
#
# Deliberately NOT pinning CMAKE_OSX_DEPLOYMENT_TARGET to 15.6 here to "make
# sure": it would silence a warning that is not being emitted, at the cost of
# a floor this project has no other reason to set. If someone builds on an
# older macOS and does see it, the fix belongs in their toolchain file, not in
# this project's unconditional settings.

# PLAN.md Phase 5 step 13, the fixture-flag audit. §6.3 house rule 3 -- "a test
# that was defined and never called has shipped on this project before" -- has
# a second form one level out: `--comps-demo` parsed its arguments into locals
# and `runCompsDemo()` sat in main.cpp's anonymous namespace, and *nothing ever
# called it*, so every step 12 screenshot photographed a document with zero
# comps. Both halves of that defect are diagnosable by the compiler, and
# neither warning is on by default in a build that sets no warning flags at
# all, which is what this target was until now:
#
#   -Wunused-function          the anonymous-namespace helper nobody calls
#   -Wunused-but-set-variable  the parsed flag nobody reads
#   -Wunused-variable          the declared-and-never-touched case between them
#
# Errors rather than warnings, deliberately. House rule 4 already requires
# "zero warnings naming a src/ path", so a warning here is a build that is
# already considered broken -- making it fail the build is the rule enforced by
# the toolchain instead of by the reviewer's eyes, which is the whole point of
# a permanent guard. Scoped to this target, so third_party's SDL/ImGui/wgpu
# targets are untouched; third_party *headers* are included as SYSTEM above,
# so stb_image.h's unused statics cannot fail a build over code this project
# does not own.
#
# The escape hatch for a genuinely-intentional unused symbol is [[maybe_unused]]
# on the declaration, which is local, visible in the diff, and says why -- not a
# flag removed from this file.
target_compile_options(naturalPaint PRIVATE
  -Werror=unused-function
  -Werror=unused-variable
  -Werror=unused-but-set-variable
)

# goldentool -- the comparison half of the golden-image regression harness
# (see tools/golden/run_golden.sh and src/tools/GoldenTool.cpp). Deliberately
# its own executable, not folded into `naturalPaint`: it never touches
# GPU/SDL/ImGui, only PNG bytes already on disk, so it has no reason to link
# any of that and every reason not to -- a comparison tool that could fail to
# build for a GPU/windowing reason would be a strange failure mode for
# something whose whole job is being trustworthy after the app already ran.
add_executable(goldentool tools/GoldenTool.cpp)
target_include_directories(goldentool SYSTEM PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party
)
