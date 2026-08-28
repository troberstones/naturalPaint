include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- SDL3
# SDL3 gives us windowing plus, critically, SDL_PenEvent: real tablet
# pressure / tilt / barrel-rotation without per-platform code.
set(SDL_SHARED   OFF CACHE BOOL "" FORCE)
set(SDL_STATIC   ON  CACHE BOOL "" FORCE)
set(SDL_TEST     OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.2.24
  GIT_SHALLOW    ON
)

# ---------------------------------------------------------------- Dear ImGui
# The docking branch's WebGPU backend targets the unified webgpu.h that
# wgpu-native v29 ships, and supports the WGPU (as opposed to Dawn) path.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.9b-docking
  GIT_SHALLOW    ON
)

FetchContent_MakeAvailable(SDL3 imgui)

# ---------------------------------------------------------------- wgpu-native
# A 13 MB prebuilt binary, vendored in third_party/wgpu. Dawn is the reference
# implementation and has better WGSL diagnostics, but it is a Chromium project:
# a shallow clone plus its dependencies runs to ~5 GB to obtain a library that
# compiles to about 40 MB. Not a trade worth making here.
#
# Pinned to v25.0.2.2 deliberately. webgpu.h is still moving: v27 renamed
# WGPUProgrammableStageDescriptor and merged two WGPUSurfaceGetCurrentTextureStatus
# enums, which Dear ImGui's WebGPU backend has not yet caught up to. v25 is the
# newest release whose header satisfies both ImGui and this codebase. Revisit
# when the backend updates.
set(WGPU_VERSION v25.0.2.2)
set(WGPU_DIR ${CMAKE_SOURCE_DIR}/third_party/wgpu)
if(NOT EXISTS ${WGPU_DIR}/lib/libwgpu_native.a)
  message(FATAL_ERROR
    "wgpu-native not found at ${WGPU_DIR}.\n"
    "Fetch it with:\n"
    "  curl -L -o /tmp/wgpu.zip https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_VERSION}/wgpu-macos-aarch64-release.zip\n"
    "  unzip -o /tmp/wgpu.zip -d ${WGPU_DIR}\n"
    "This build then hashes the extracted ${WGPU_DIR}/lib/libwgpu_native.a and refuses to "
    "configure if it does not match the pin below -- see that check's own comment for what it "
    "does and does NOT protect against.")
endif()

# docs/architecture-review.md P2-2 item 5: the two lines above told the user to
# pipe a `curl` straight into `unzip` with nothing checked afterward. Anyone
# who can MITM that download, or compromise the release asset at GitHub, gets
# arbitrary code linked into every build -- silently, since a swapped-out
# static library changes no source file this repo tracks.
#
# **What this checks, and what it deliberately does not.** The pin below is
# the SHA-256 of the EXTRACTED `libwgpu_native.a` this build actually links --
# not of `wgpu-macos-aarch64-release.zip`, the archive the `curl` line above
# downloads. That is not a simplification made for convenience: the `.zip` is
# not a build input CMake ever touches (the user's shell unzips it, by hand,
# before CMake is invoked at all), so there is no configure-time hook this
# file could attach a zip check to even if one were wanted. Hashing the `.a`
# instead checks the exact bytes the linker reads, regardless of how they got
# into `third_party/wgpu/lib/` -- a tampered zip, a MITM'd `curl`, or the file
# swapped out by hand after extraction all fail the same way. What it does
# NOT do is stop a hostile `.a` from being unzipped onto disk in the first
# place, or from `unzip` itself running against tampered bytes; the refusal
# only fires the next time CMake configures, after the damage of extracting
# an untrusted archive is already done. Checking the zip pre-extraction would
# close that gap and is worth doing if this project starts scripting the
# fetch step instead of asking the user to run `curl | unzip` by hand.
#
# Computed directly against the artifact vendored in this tree with
# `shasum -a 256 third_party/wgpu/lib/libwgpu_native.a` -- not invented, not
# copied from the zip's own (unpublished, unchecked) checksum, since the zip
# was not available to hash. Re-derive this pin, with the same command,
# whenever WGPU_VERSION above changes.
set(WGPU_NATIVE_A_SHA256 ae3b0ae457862e0616d71d690947bb8978b247c4c26c866d5aabaa9a8bfe1b55)
file(SHA256 ${WGPU_DIR}/lib/libwgpu_native.a WGPU_NATIVE_A_ACTUAL_SHA256)
if(NOT WGPU_NATIVE_A_ACTUAL_SHA256 STREQUAL WGPU_NATIVE_A_SHA256)
  message(FATAL_ERROR
    "${WGPU_DIR}/lib/libwgpu_native.a does not match the pinned SHA-256 for "
    "wgpu-native ${WGPU_VERSION}.\n"
    "  expected: ${WGPU_NATIVE_A_SHA256}\n"
    "  actual:   ${WGPU_NATIVE_A_ACTUAL_SHA256}\n"
    "This is refused rather than linked: either the download was corrupted or "
    "intercepted, or WGPU_VERSION above moved without this pin being updated to "
    "match (re-derive it with `shasum -a 256` against the new .a and update "
    "WGPU_NATIVE_A_SHA256 in cmake/Dependencies.cmake). Do not silence this by "
    "deleting the check -- delete third_party/wgpu and re-fetch instead.")
endif()

add_library(wgpu_native STATIC IMPORTED GLOBAL)
set_target_properties(wgpu_native PROPERTIES
  IMPORTED_LOCATION ${WGPU_DIR}/lib/libwgpu_native.a
  INTERFACE_INCLUDE_DIRECTORIES ${WGPU_DIR}/include
)
if(APPLE)
  # wgpu-native's Metal backend pulls these in; a static lib cannot carry them.
  target_link_libraries(wgpu_native INTERFACE
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework AppKit"
  )
endif()

# ---------------------------------------------------------------- imgui target
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
)
# imgui_impl_wgpu.cpp carries a Cocoa surface-creation helper we don't use (SDL
# makes our surface), but it still has to parse. Upstream documents this flag.
if(APPLE)
  set_source_files_properties(${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
    PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
  IMGUI_IMPL_WEBGPU_BACKEND_WGPU
  IMGUI_DEFINE_MATH_OPERATORS
)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static wgpu_native)
