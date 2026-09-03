add_executable(naturalPaint
  main.cpp
  app/BrushLibraryFile.cpp
  app/DabLibrary.cpp
  app/AbrReport.cpp
  app/BrushSheet.cpp
  app/BrushRowIcon.cpp
  app/CloseDecision.cpp
  app/CompPanel.cpp
  app/PanelLayout.cpp
  app/ControlsLayout.cpp
  app/CurveEdit.cpp
  app/DabPreview.cpp
  app/DocumentLifecycle.cpp
  app/ImportImage.cpp
  app/Journal.cpp
  app/HistoryPanel.cpp
  app/Keymap.cpp
  app/PenAxes.cpp
  app/PsdReport.cpp
  app/ProfileToggle.cpp
  app/Screenshot.cpp
  app/SelectionDrag.cpp
  app/LayerEditor.cpp
  app/TransformSession.cpp
  app/MoveTool.cpp
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
  app/selftest/AdjustmentMenu.cpp
  app/selftest/AdjustmentLayer.cpp
  app/selftest/ApplyPass.cpp
  app/selftest/BaseLayerAlpha.cpp
  app/selftest/AbrBrushes.cpp
  app/selftest/AbrDualBrush.cpp
  app/selftest/AbrSampledTips.cpp
  app/selftest/PsPatterns.cpp
  app/selftest/GimpBrush.cpp
  app/selftest/Variance.cpp
  app/selftest/CoverageBlend.cpp
  app/selftest/PaperTexture.cpp
  app/selftest/DabLibrary.cpp
  app/selftest/PatternExtract.cpp
  app/selftest/DabPicker.cpp
  app/selftest/BrushSettingsWindow.cpp
  app/selftest/BrushModelIo.cpp
  app/selftest/BrushModelDiff.cpp
  app/selftest/BrushPanelBinding.cpp
  app/selftest/AngleConvention.cpp
  app/selftest/Blend.cpp
  app/selftest/Blur.cpp
  app/selftest/BlurSimd.cpp
  app/selftest/BrushDynamics.cpp
  app/selftest/BrushLibraryFile.cpp
  app/selftest/BucketRefusal.cpp
  app/selftest/CanvasDimensions.cpp
  app/selftest/Channels.cpp
  app/selftest/CheckedAdd.cpp
  app/selftest/ChromeConsistency.cpp
  app/selftest/Clipboard.cpp
  app/selftest/ClipboardImage.cpp
  app/selftest/ClippingMask.cpp
  app/selftest/CloneStamp.cpp
  app/selftest/CloseDecision.cpp
  app/selftest/ColorOps.cpp
  app/selftest/ColorSpace.cpp
  app/selftest/CompositeCost.cpp
  app/selftest/OpaqueFloor.cpp
  app/selftest/CompositeParallel.cpp
  app/selftest/ViewportDeferredComposite.cpp
  app/selftest/PanelLayout.cpp
  app/selftest/DockLayout.cpp
  app/selftest/ControlsLayout.cpp
  app/selftest/CowTile.cpp
  app/selftest/CreateBlank.cpp
  app/selftest/CurveEdit.cpp
  app/selftest/DabPreview.cpp
  app/selftest/Descriptor.cpp
  app/selftest/Document.cpp
  app/selftest/DocumentLifecycle.cpp
  app/selftest/DocumentPresets.cpp
  app/selftest/DocumentResidency.cpp
  app/selftest/DocumentTexture.cpp
  app/selftest/DocumentTransform.cpp
  app/selftest/Driver.cpp
  app/selftest/DynamicsSources.cpp
  app/selftest/EllipseMarqueePreview.cpp
  app/selftest/Export.cpp
  app/selftest/ExportAs.cpp
  app/selftest/ExportStates.cpp
  app/selftest/Eyedropper.cpp
  app/selftest/FieldAllocation.cpp
  app/selftest/FileDialog.cpp
  app/selftest/FilterMenu.cpp
  app/selftest/Filters.cpp
  app/selftest/FiltersExt.cpp
  app/selftest/FloodFill.cpp
  app/selftest/Fonts.cpp
  app/selftest/AutoLevels.cpp
  app/selftest/AtelierChrome.cpp
  app/selftest/ActiveLayer.cpp
  app/selftest/FormatSupport.cpp
  app/selftest/GradeDispatch.cpp
  app/selftest/Grain.cpp
  app/selftest/Gradient.cpp
  app/selftest/GuidesGridSnap.cpp
  app/selftest/Half.cpp
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
  app/selftest/LayerGroup.cpp
  app/selftest/LayerGroupPanel.cpp
  app/selftest/LayerMask.cpp
  app/selftest/LayerMultiSelect.cpp
  app/selftest/LayerPanel2a.cpp
  app/selftest/LayerStack.cpp
  app/selftest/LutBake.cpp
  app/selftest/Measure.cpp
  app/selftest/MenuBasics.cpp
  app/selftest/MenuModel.cpp
  app/selftest/MonoOps.cpp
  app/selftest/MergeFamily.cpp
  app/selftest/MipPyramid.cpp
  app/selftest/MultiplyFloor.cpp
  app/selftest/NpaintFormat.cpp
  app/selftest/OpenAnyFile.cpp
  app/selftest/OpStack.cpp
  app/selftest/Parallel.cpp
  app/selftest/PigmentBake.cpp
  app/selftest/PigmentBasis.cpp
  app/selftest/PigmentDeposit.cpp
  app/selftest/PigmentLayer.cpp
  app/selftest/PigmentSelection.cpp
  app/selftest/PlaceImageAsLayer.cpp
  app/selftest/PointOps.cpp
  app/selftest/PresentTransfer.cpp
  app/selftest/PressureFeel.cpp
  app/selftest/PencilDeposit.cpp
  app/selftest/Probe.cpp
  app/selftest/PackBits.cpp
  app/selftest/PsdImport.cpp
  app/selftest/QuitGuard.cpp
  app/selftest/RecoveryJournal.cpp
  app/selftest/ResamplePerf.cpp
  app/selftest/ResourcePaths.cpp
  app/selftest/RgbDeposit.cpp
  app/selftest/RgbErase.cpp
  app/selftest/Smudge.cpp
  app/selftest/SaveReadback.cpp
  app/selftest/Scatter.cpp
  app/selftest/ScatterCount.cpp
  app/selftest/Selection.cpp
  app/selftest/SelectionBoundary.cpp
  app/selftest/SelectionDrag.cpp
  app/selftest/SelectionRefine.cpp
  app/selftest/SelectionShapes.cpp
  app/selftest/SelectionTools.cpp
  app/selftest/SelectMenu.cpp
  app/selftest/Shaper.cpp
  app/selftest/ShelvedLinks.cpp
  app/selftest/SolverPersistence.cpp
  app/selftest/StrokePreview.cpp
  app/selftest/StrokeBridge.cpp
  app/selftest/StrokeSpeed.cpp
  app/selftest/Support.cpp
  app/selftest/TileResidency.cpp
  app/selftest/TileStore.cpp
  app/selftest/ToneOps.cpp
  app/selftest/ToolCursor.cpp
  app/selftest/TonalBrush.cpp
  app/selftest/ToolOptionsBlend.cpp
  app/selftest/TouchGesture.cpp
  app/selftest/TouchGestureSession.cpp
  app/selftest/TransferDynamics.cpp
  app/selftest/Transform.cpp
  app/selftest/TransformCompositeSplit.cpp
  app/selftest/TransformPreviewTexture.cpp
  app/selftest/TransformSession.cpp
  app/selftest/GradientTool.cpp
  app/selftest/MoveTool.cpp
  app/selftest/PathRaster.cpp
  app/selftest/SvgPath.cpp
  app/selftest/VectorLayer.cpp
  app/selftest/UserBrushLibrary.cpp
  app/selftest/ViewTransform.cpp
  app/selftest/WheelInput.cpp
  app/selftest/ZoomAndSize.cpp
  app/AdjustmentOps.cpp
  app/FilterOps.cpp
  app/MeasureLine.cpp
  app/GradientTool.cpp
  app/OpenAnyFile.cpp
  app/QuitSequence.cpp
  app/Snapping.cpp
  app/StrokeBake.cpp
  app/StrokePreview.cpp
  app/StrokeSession.cpp
  app/DocumentPresets.cpp
  app/TouchGesture.cpp
  app/TouchGestureSession.cpp
  app/UserBrushLibrary.cpp
  app/ViewTransform.cpp
  app/WheelInput.cpp
  app/ZoomAndSize.cpp
  brush/BrushModel.cpp
  brush/BrushModelIo.cpp
  brush/BrushModelDiff.cpp
  brush/CloneStamp.cpp
  brush/CoverageBlend.cpp
  brush/Deposit.cpp
  brush/Dynamics.cpp
  brush/Grain.cpp
  brush/Library.cpp
  brush/PencilDeposit.cpp
  brush/PigmentErase.cpp
  brush/RgbDeposit.cpp
  brush/RgbErase.cpp
  brush/Smudge.cpp
  brush/StrokePath.cpp
  brush/TonalBrush.cpp
  brush/ToolOptionsBlend.cpp
  brush/Variance.cpp
  color/LutBake.cpp
  color/Shaper.cpp
  color/Space.cpp
  core/Blend.cpp
  core/Channels.cpp
  core/Clipboard.cpp
  core/Composite.cpp
  core/DirtyTiles.cpp
  core/Histogram.cpp
  core/History.cpp
  core/LayerCompOps.cpp
  core/LayerGeometry.cpp
  core/LayerOps.cpp
  core/LayerSetOps.cpp
  core/Merge.cpp
  core/OpStack.cpp
  core/Path.cpp
  core/PathFlatten.cpp
  core/PathRaster.cpp
  core/PathStroke.cpp
  core/VectorRaster.cpp
  core/VectorShape.cpp
  core/Pigment.cpp
  core/PigmentBake.cpp
  core/Probe.cpp
  core/ResourcePaths.cpp
  core/SelectionBoundary.cpp
  core/SelectionMask.cpp
  core/SelectionOps.cpp
  core/SelectionRefine.cpp
  core/SelectionShapes.cpp
  gfx/Context.cpp
  gfx/ShaderLoader.cpp
  io/AbrBrushes.cpp
  io/Capabilities.cpp
  io/ClipboardImage.cpp
  io/CompSerial.cpp
  io/Descriptor.cpp
  io/Export.cpp
  io/ExportAs.cpp
  io/ExportStates.cpp
  io/FileKind.cpp
  io/GimpBrush.cpp
  io/ImageDecode.cpp
  io/ImageIO.cpp
  io/NpaintFile.cpp
  io/OpSerial.cpp
  io/PackBits.cpp
  io/PathSerial.cpp
  io/SvgPath.cpp
  io/PsPatterns.cpp
  io/PsdImport.cpp
  io/TileResidency.cpp
  ops/AutoLevels.cpp
  ops/Blur.cpp
  ops/ColorOps.cpp
  ops/DocumentTransform.cpp
  ops/Feather.cpp
  ops/Filters.cpp
  ops/FloodFill.cpp
  ops/Gradient.cpp
  ops/MonoOps.cpp
  ops/PointOpTiles.cpp
  ops/PointOps.cpp
  ops/Resample.cpp
  ops/Roi.cpp
  ops/ToneOps.cpp
  ops/Transform.cpp
  paint/Palette.cpp
  sim/PaintSim.cpp
  ui/DocumentTexture.cpp
  ui/LabelledControl.cpp
  ui/DynamicsMatrixPanel.cpp
  ui/MacPaintUI.cpp
  ui/NaturalPaintUI.cpp
  ui/Fonts.cpp
  ui/AtelierTheme.cpp
  ui/CanvasQuad.cpp
  ui/DabPicker.cpp
  ui/BrushSettingsWindow.cpp
  ui/BrushFieldPresentation.cpp
  ui/FileDialog.cpp
  ui/AtelierLayout.cpp
  ui/DockLayout.cpp
  ui/AtelierChrome.cpp
  ui/MenuModel.cpp
  ui/NewDocumentDialog.cpp
  ui/ToolCursor.cpp
  ui/TransformCompositeSplit.cpp
  ui/TransformPreviewTexture.cpp
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

  # ---- pinch-to-zoom (ui/MacTrackpadGestures.mm) -----------------------------
  #
  # track10/input: SDL 3.2.24 as vendored here has no pinch/magnification event
  # on any platform (see ui/MacTrackpadGestures.hpp's header comment for what
  # was checked before reaching this conclusion), so this is the same "reach
  # past SDL for one AppKit feature" pattern as the native menu bar immediately
  # above -- same language, same ARC rationale (a local `NSEvent` monitor block
  # captured for the app's whole lifetime), same explicit Cocoa link for the
  # same reason (this file `#import`s <Cocoa/Cocoa.h> directly).
  target_sources(naturalPaint PRIVATE ui/MacTrackpadGestures.mm)
  set_source_files_properties(ui/MacTrackpadGestures.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  message(STATUS "naturalPaint: native pinch-to-zoom (ui/MacTrackpadGestures.mm)")

  # ---- raw two-finger touch capture (ui/MacTrackpadTouch.mm) ----------------
  #
  # item 4 ("trackpad interactions feel unnatural"): bypasses AppKit's own
  # magnify/rotate gesture classifier by reading NSTouch positions directly --
  # see ui/MacTrackpadTouch.hpp's header comment for the full reasoning (why
  # this needs the real NSWindow, unlike the local-monitor pollers immediately
  # above). Same language/ARC/Cocoa-link rationale as both files above; this
  # one additionally needs SDL3's own header for SDL_GetWindowProperties() and
  # SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, already linked into this target.
  target_sources(naturalPaint PRIVATE ui/MacTrackpadTouch.mm)
  set_source_files_properties(ui/MacTrackpadTouch.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  message(STATUS "naturalPaint: native raw two-finger touch capture (ui/MacTrackpadTouch.mm)")
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

# ---- pugixml (vendored, third_party/pugixml) --------------------------------
#
# The XML parser for io/SvgImport. Vendored rather than fetched, at v1.14, with
# the three upstream files byte-identical to the tag (third_party/pugixml/
# VERSION.md records the hashes, so "is this really upstream?" is a one-command
# question rather than a matter of trust).
#
# **Why this parser and not a smaller hand-rolled one.** An SVG is a file this
# build did not write, and the two classic XML attacks are both *parser
# features* being abused: XXE needs external entity resolution, and the
# billion-laughs expansion needs a DTD-declared entity table. pugixml has
# neither -- `strconv_escape()` expands exactly `&#...;`, `&amp;`, `&apos;`,
# `&gt;`, `&lt;` and `&quot;` and nothing else, and the string `SYSTEM` does not
# appear in its source at all. So both attacks are impossible by construction
# rather than by hardening we would have to keep correct ourselves. That is the
# decisive argument; being MIT and three files is only why it is cheap.
#
# It is its own target, not source files added to `naturalPaint`, so that the
# `-Werror=unused-*` guard below judges only code this project owns -- the same
# reason third_party headers are included SYSTEM. The two hardening flags are
# repeated here deliberately: this target is the one that touches the untrusted
# bytes first, so it is the last place that should miss them.
#
# XPath is compiled out. Nothing here queries by XPath -- the importer walks the
# tree directly -- and it is a large fraction of pugixml.cpp.
add_library(pugixml STATIC
  ${CMAKE_SOURCE_DIR}/third_party/pugixml/pugixml.cpp
)
target_include_directories(pugixml SYSTEM PUBLIC
  ${CMAKE_SOURCE_DIR}/third_party
)
target_compile_definitions(pugixml PUBLIC PUGIXML_NO_XPATH)
target_compile_options(pugixml PRIVATE
  -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST
  -fstack-protector-strong
)
set_target_properties(pugixml PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(naturalPaint PRIVATE
  SDL3::SDL3-static
  wgpu_native
  imgui
  pugixml
)

# Shaders, the pigment LUT, the keymap and the Lucide icon font are read from
# disk at runtime rather than compiled in, so the solver can be edited and
# reloaded (Cmd+R) and the keymap hand-edited, without a rebuild -- same
# convention for all four.
#
# These five macros are now `core/ResourcePaths.cpp`'s tier-3 fallback, not
# the path callers use directly (docs/architecture-review.md P1-2, "The
# binary cannot leave the machine that built it": a raw CMAKE_SOURCE_DIR
# path is an absolute path into *this* machine's checkout, which breaks the
# moment the binary is copied elsewhere -- worst case silently, for the
# Lucide font, per that finding). `core/ResourcePaths.hpp` has the full
# three-tier search order and the reasoning for it; every one of the five
# `NP_*` values below still needs to exist, unconditionally, because tier 3
# is what keeps a developer running straight out of `build/src/` -- with
# nothing staged, or an old build predating staging -- seeing no change at
# all.
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

# Stages tier 2 of core/ResourcePaths.hpp's search order: a copy of the same
# five resources, in the same relative layout the source tree uses, next to
# the built executable. Without this, `core/ResourcePaths.cpp`'s
# executable-relative lookup would have nothing to find and every run would
# silently fall through to tier 3 (the compile-time path) -- a resolver with
# an unreachable middle tier is not a portability fix, it just moves where
# the machine-specific path is written down. `$<TARGET_FILE_DIR:naturalPaint>`
# rather than a hard-coded `src/` so this keeps working if the target's
# output directory ever changes.
#
# `copy_directory` for the two directories (`shaders/`, `keymaps/`) and
# `copy` for the three individual files, run as one POST_BUILD step so a
# fresh build always leaves `build/src/` self-contained -- this is also
# exactly what the "prove the binary can leave this machine" check copies
# elsewhere, so if this step is wrong that check fails immediately rather
# than the gap surviving until someone actually ships the binary.
add_custom_command(TARGET naturalPaint POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:naturalPaint>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/keymaps" "$<TARGET_FILE_DIR:naturalPaint>/keymaps"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:naturalPaint>/third_party/mixbox/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_SOURCE_DIR}/third_party/mixbox/shaders/mixbox_lut.png"
          "$<TARGET_FILE_DIR:naturalPaint>/third_party/mixbox/shaders/mixbox_lut.png"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:naturalPaint>/third_party/lucide"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_SOURCE_DIR}/third_party/lucide/lucide.ttf"
          "$<TARGET_FILE_DIR:naturalPaint>/third_party/lucide/lucide.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_SOURCE_DIR}/third_party/lucide/codepoints.json"
          "$<TARGET_FILE_DIR:naturalPaint>/third_party/lucide/codepoints.json"
  COMMENT "naturalPaint: staging shaders/keymaps/mixbox-lut/lucide beside the built executable (core/ResourcePaths tier 2)"
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
  # **`-Werror=switch`, and it is load-bearing for comments already in the
  # tree.** Several files argue that a `switch` over a closed enum with no
  # `default:` makes adding an enumerator a COMPILE error -- `sourceUnavailable()`
  # and `easingPresetName()` in `brush/Dynamics.cpp`, `combineDualCoverage()` in
  # `brush/Deposit.cpp`, the LINK editor's chip tooltips in
  # `ui/DynamicsMatrixPanel.cpp`.
  # That was **false** until this line: clang enables `-Wswitch` by default, but
  # a warning nobody reads is not a guard, and this build elevates only the three
  # `unused-*` diagnostics above. A subagent tested it directly -- commented out a
  # case, rebuilt, exit 0 -- which is how the gap was found rather than assumed.
  #
  # Adding it cost nothing: the tree already compiled clean under it, so no
  # existing switch was relying on falling through to a trailing return. Proved
  # it BITES rather than merely building: deleting `EasingPreset::LogTaper`'s
  # case from `easingPresetName()` now fails with
  # `error: enumeration value 'LogTaper' not handled in switch [-Werror,-Wswitch]`.
  #
  # The escape hatch is a `default:` on the one switch that genuinely wants to
  # handle enumerators generically -- local, visible in the diff, and it says so
  # -- not this flag removed from this file.
  -Werror=switch
)

# ---- hardening (docs/architecture-review.md P2-2 item 6) --------------------
#
# This binary decodes ABR, PSD, EXR, PNG and JPEG from files it did not write
# (io/AbrBrushes.cpp, io/ImageDecode.cpp, io/OiioBackend.cpp) -- exactly the
# code the parsers' own bounds checks (io/Descriptor.hpp, io/AbrBrushes.cpp's
# `checkedAdd`) are written to keep correct by hand. These two flags are a
# second, independent line of defence that does not depend on any one parser
# getting its own arithmetic right:
#
# `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` turns an
# out-of-bounds `std::vector`/`std::array`/`std::span` index -- anywhere in
# this target, not only in the files this review pass touched -- into an
# abort instead of silently reading or writing past the end. "Fast" mode is
# libc++'s name for the subset of its checks documented to cost close to
# nothing (no iterator-invalidation tracking, no debug-mode ABI change),
# which is what makes this a default rather than a diagnostic build variant
# nobody enables. `-fstack-protector-strong` puts a canary in front of any
# stack frame with an array or an address-taken local, catching a stack
# buffer overflow before it overwrites a return address rather than after.
#
# **"Near-zero cost" was the review's own phrasing. It is wrong, and the
# measurement is here rather than the phrase.** `--selftest`'s own composite
# and blur `[measured]` lines, three runs per configuration on this machine
# (M4 Max), four separate builds -- neither flag, each alone, and both:
#
#     composite         none      libc++    ssp       both
#     1 tile          0.1405 ms  0.1443    0.1536    0.1544  (+9.9%)
#     4-tile dab      0.5648 ms  0.5777    0.6173    0.6163  (+9.1%)
#     8 tiles         1.1279 ms  1.1631    1.2426    1.2452  (+10.4%)
#     blur 1024^2      241.3 ms   --        --        235.3  (no change)
#     blur 2048^2      928.2 ms   --        --        920.7  (no change)
#
# Every hardened composite reading sits above every unhardened one, so the
# ~10% is a real separation and not the run-to-run noise this project already
# documents. Splitting the two flags shows where it comes from: libc++'s FAST
# mode costs 2.3-2.7%, and `-fstack-protector-strong` costs the other ~7%
# essentially by itself. Blur does not move at all -- it is float arithmetic
# over raw pointers, with neither a container index nor an array-bearing
# stack frame in its inner loop.
#
# It is still landed as a default, on the absolute number rather than the
# relative one: the interactive path is the dab, and the dab pays **0.05 ms
# of PRD F3's 20 ms pen-to-photon budget** -- 0.3% of a frame. The 10% is
# only alarming as a ratio. If a future change makes the full-canvas
# recomposite (11.3 -> 12.4 ms) the thing standing between this app and F3,
# the honest first move is to drop `-fstack-protector-strong` from
# `core/Composite.cpp` alone via `set_source_files_properties()`, keeping it
# where the untrusted bytes are, rather than to turn hardening off globally.
target_compile_options(naturalPaint PRIVATE
  -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST
  -fstack-protector-strong
)

# ---- link-time optimisation (docs/architecture-review.md P0-2) --------------
#
# The per-pixel inner loop of `core/Composite.cpp` calls `blendPixel()` (in
# `Blend.cpp`, with a ~27-mode switch inside) and, for adjustment layers,
# `applyPointOpsPremultiplied()` (in `PointOps.cpp`). Both are in other
# translation units, so without LTO neither can be inlined, the blend-mode
# switch cannot be hoisted out of a loop it is constant across, and nothing
# vectorises over the call boundary. For a 4096^2 document with ten
# full-canvas layers the review counts roughly 840 million non-inlinable calls
# per full composite.
#
# P0-1 already removed the worst offender by making `core/Half.hpp`
# header-only -- that one was on EVERY pixel path, not just the composite, and
# it did not need LTO because a header needs no link step to be inlined. This
# catches the rest.
#
# Guarded by `check_ipo_supported()` rather than set unconditionally: a
# toolchain without it should build a working binary, not fail to configure.
# The RESULT/OUTPUT pair means a refusal is reported with the compiler's own
# reason instead of silently producing an un-optimised build -- the same
# "make the degradation loud" rule ui/Fonts.cpp's missing-font path exists for.
include(CheckIPOSupported)
check_ipo_supported(RESULT np_ipo_ok OUTPUT np_ipo_why)
if(np_ipo_ok)
  set_property(TARGET naturalPaint PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  message(STATUS "naturalPaint: link-time optimisation enabled (P0-2)")
else()
  message(STATUS "naturalPaint: link-time optimisation NOT available -- ${np_ipo_why}")
endif()

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
