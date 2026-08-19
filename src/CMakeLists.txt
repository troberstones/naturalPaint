add_executable(naturalPaint
  main.cpp
  app/CurveEdit.cpp
  app/DocumentLifecycle.cpp
  app/Keymap.cpp
  app/Latency.cpp
  app/Memory.cpp
  app/SelfTest.cpp
  app/Snapping.cpp
  app/ViewTransform.cpp
  brush/StrokePath.cpp
  color/LutBake.cpp
  color/Shaper.cpp
  color/Space.cpp
  core/Half.cpp
  core/Histogram.cpp
  core/OpStack.cpp
  core/Probe.cpp
  gfx/Context.cpp
  gfx/ShaderLoader.cpp
  io/Capabilities.cpp
  io/Export.cpp
  io/ExportAs.cpp
  io/ImageDecode.cpp
  io/ImageIO.cpp
  io/NpaintFile.cpp
  io/TileResidency.cpp
  ops/PointOps.cpp
  ops/Resample.cpp
  paint/Palette.cpp
  sim/PaintSim.cpp
  ui/MacPaintUI.cpp
  ui/NaturalPaintUI.cpp
  ui/Theme.cpp
)

target_include_directories(naturalPaint PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
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
)

if(NP_USE_MIXBOX)
  target_compile_definitions(naturalPaint PRIVATE NP_USE_MIXBOX=1)
endif()

# PLAN.md phase 4 step 2/3. io/OiioBackend.cpp is the *only* translation unit
# that includes an OpenImageIO header, and it is added to the target only
# here -- so with NP_USE_OIIO=OFF the binary contains no OpenImageIO symbol
# at all, which is PRD I3's "the core builds and runs without OIIO" enforced
# by the link step rather than by convention. Everything else (io/Export.cpp,
# io/Capabilities.cpp, io/ImageDecode.cpp) guards its include of
# io/OiioBackend.hpp with `#if defined(NP_USE_OIIO)` and compiles unchanged
# either way.
#
# Both OpenImageIO::OpenImageIO and OpenImageIO::OpenImageIO_Util are linked
# explicitly. The config package's imported target does chain the Util
# library, but naming it here is deliberate rather than redundant: ustring's
# symbols (e.g. ustring::empty_std_string) live in the Util library only, and
# an OpenImageIO install whose imported target did not chain it would fail at
# link time with an error that points nowhere near the cause.
if(NP_USE_OIIO)
  find_package(OpenImageIO REQUIRED)
  target_sources(naturalPaint PRIVATE io/OiioBackend.cpp)
  target_compile_definitions(naturalPaint PRIVATE NP_USE_OIIO=1)
  target_link_libraries(naturalPaint PRIVATE
    OpenImageIO::OpenImageIO
    OpenImageIO::OpenImageIO_Util
  )
  message(STATUS "naturalPaint: OpenImageIO ${OpenImageIO_VERSION} backend enabled")
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
  # Deliberately NOT pinning CMAKE_OSX_DEPLOYMENT_TARGET to 15.6 here to
  # "make sure": that would raise the floor for the NP_USE_OIIO=OFF
  # configuration too, which has no OpenImageIO to be compatible with, in
  # exchange for silencing a warning that is not being emitted. If someone
  # builds on an older macOS and does see it, the fix belongs in their
  # toolchain file, not in this project's unconditional settings.
endif()
