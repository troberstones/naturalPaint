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
if(NP_PLATFORM_IOS)

# iOS (device or simulator): fetched at configure time, the same
# `file(DOWNLOAD ... )` + hash-the-extracted-`.a` shape the non-Apple branch
# below uses, rather than the vendored-and-hand-fetched macOS branch above --
# there is no reasonable "vendor it in the repo" answer for two more
# arm64 slices (device vs. simulator) of the same 13 MB library. Which asset
# is right depends on CMAKE_OSX_SYSROOT, which the caller sets when they pass
# `-DCMAKE_OSX_SYSROOT=iphonesimulator` or `iphoneos`
# (docs/ios-spike-plan.md Phase 0's CMake invocation). Both `.a` hashes below
# were verified independently twice against wgpu-native ${WGPU_VERSION}'s own
# release assets, the same way the Apple branch's pin was derived --
# `shasum -a 256` against the extracted library, not the zip.
if(CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
  set(WGPU_IOS_ASSET wgpu-ios-aarch64-simulator-release.zip)
  set(WGPU_IOS_A_SHA256 b17d56b8e2e82b9688abfaa873bbd2de0c28bc9838105142470ff2cb68748d7f)
else()
  set(WGPU_IOS_ASSET wgpu-ios-aarch64-release.zip)
  set(WGPU_IOS_A_SHA256 72dc2354c0a141bc59f646ba13eb6166dac45eb7231cd167a74b9d740ee998aa)
endif()
set(WGPU_IOS_DIR ${CMAKE_BINARY_DIR}/wgpu-native)
set(WGPU_IOS_ZIP ${WGPU_IOS_DIR}/${WGPU_IOS_ASSET})
set(WGPU_NATIVE_A ${WGPU_IOS_DIR}/lib/libwgpu_native.a)
if(NOT EXISTS ${WGPU_NATIVE_A})
  file(DOWNLOAD
    https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_VERSION}/${WGPU_IOS_ASSET}
    ${WGPU_IOS_ZIP}
    TLS_VERIFY ON
  )
  file(ARCHIVE_EXTRACT INPUT ${WGPU_IOS_ZIP} DESTINATION ${WGPU_IOS_DIR})
endif()
if(NOT EXISTS ${WGPU_NATIVE_A})
  message(FATAL_ERROR
    "Extracted ${WGPU_IOS_ZIP} but ${WGPU_NATIVE_A} is not there -- the "
    "release asset's internal layout changed. Re-check wgpu-native "
    "${WGPU_VERSION}'s ${WGPU_IOS_ASSET} by hand.")
endif()
file(SHA256 ${WGPU_NATIVE_A} WGPU_IOS_A_ACTUAL_SHA256)
if(NOT WGPU_IOS_A_ACTUAL_SHA256 STREQUAL WGPU_IOS_A_SHA256)
  message(FATAL_ERROR
    "${WGPU_NATIVE_A} does not match the pinned SHA-256 for wgpu-native "
    "${WGPU_VERSION}'s ${WGPU_IOS_ASSET}.\n"
    "  expected: ${WGPU_IOS_A_SHA256}\n"
    "  actual:   ${WGPU_IOS_A_ACTUAL_SHA256}\n"
    "Refused rather than linked, for the same reason the macOS/Linux "
    "branches refuse: delete ${WGPU_IOS_DIR} and re-fetch instead of "
    "silencing this.")
endif()

elseif(APPLE)
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
# fetch step instead of asking the user to run `curl | unzip` by hand -- which
# is exactly what the non-Apple branch below does, precisely because it IS
# CMake doing the fetching this time: `file(DOWNLOAD ... EXPECTED_HASH)`
# refuses to write a byte to disk that does not match the pin, so the zip is
# checked before extraction rather than the `.a` after it. Both branches still
# end by hashing the artifact the linker actually reads, for the same reason:
# it is the one check that catches "the file on disk right now" regardless of
# how it got there.
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

set(WGPU_NATIVE_A ${WGPU_DIR}/lib/libwgpu_native.a)

else() # non-Apple: fetch the matching Linux release into the build tree.

# No `.a` is vendored for non-Apple platforms (only third_party/wgpu/include
# is, and it is shared -- see below). Instead of asking the user to `curl |
# unzip` by hand as the Apple branch's error message above does, this does it
# at configure time with `file(DOWNLOAD ... EXPECTED_HASH)`, which is the
# stronger form of the same check: it refuses to write the zip to disk at all
# if the hash does not match, rather than writing it and hashing the result
# afterward. That closes exactly the gap the comment above says checking the
# zip pre-extraction would close. Skipped once the `.a` already exists in the
# build tree so a reconfigure does not re-download 15 MB every time.
set(WGPU_LINUX_DIR ${CMAKE_BINARY_DIR}/wgpu-native)
set(WGPU_LINUX_ZIP ${WGPU_LINUX_DIR}/wgpu-linux-x86_64-release.zip)
set(WGPU_LINUX_ZIP_SHA256 78a2a4d90f3a0a67af2ab2634fe09873ced3baceac8822e890cdea34d8ba9834)
set(WGPU_NATIVE_A ${WGPU_LINUX_DIR}/lib/libwgpu_native.a)
if(NOT EXISTS ${WGPU_NATIVE_A})
  file(DOWNLOAD
    https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_VERSION}/wgpu-linux-x86_64-release.zip
    ${WGPU_LINUX_ZIP}
    EXPECTED_HASH SHA256=${WGPU_LINUX_ZIP_SHA256}
    TLS_VERIFY ON
  )
  file(ARCHIVE_EXTRACT INPUT ${WGPU_LINUX_ZIP} DESTINATION ${WGPU_LINUX_DIR})
  if(NOT EXISTS ${WGPU_NATIVE_A})
    message(FATAL_ERROR
      "Extracted ${WGPU_LINUX_ZIP} but ${WGPU_NATIVE_A} is not there -- the "
      "release asset's internal layout changed. Re-check wgpu-native "
      "${WGPU_VERSION}'s wgpu-linux-x86_64-release.zip by hand.")
  endif()
endif()

# Headers still come from third_party/wgpu/include (see INTERFACE_INCLUDE_
# DIRECTORIES below), not from this download: the two are the same tag's
# webgpu.h, this app is written against the vendored copy, and there is no
# reason to carry two paths into the same file when only one platform's
# library differs.

endif()

add_library(wgpu_native STATIC IMPORTED GLOBAL)
set_target_properties(wgpu_native PROPERTIES
  IMPORTED_LOCATION ${WGPU_NATIVE_A}
  INTERFACE_INCLUDE_DIRECTORIES ${WGPU_DIR}/include
)
if(NP_PLATFORM_IOS)
  # Same Metal backend as macOS below, minus AppKit (which does not exist on
  # iOS) plus UIKit in its place -- verified in Phase 0 of
  # docs/ios-spike-plan.md. IOKit and IOSurface still apply: both are
  # available on iOS, and wgpu-native's Metal backend links them there too.
  target_link_libraries(wgpu_native INTERFACE
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework UIKit"
  )
elseif(APPLE)
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
else()
  # wgpu-native is a Rust static library; its Vulkan backend dlopens
  # libvulkan.so.1 at runtime rather than linking it (so a machine with no
  # Vulkan ICD still gets a defined "adapter not found" instead of a missing
  # shared object at process start), which is what `dl` is for here. `pthread`
  # and `m` are what a `.a` built by `rustc` for a glibc target needs from a
  # link line: the Rust std runtime's thread and synchronization primitives
  # resolve to libpthread symbols, and naga (wgpu-native's shader IR, statically
  # linked in) calls libm transcendentals -- `log`, `exp`, `pow`, and their
  # `f`-suffixed float forms -- directly rather than through any C++ standard
  # library wrapper.
  #
  # Measured, not guessed: linking a trivial C program against exactly this
  # downloaded `libwgpu_native.a` with no extra libraries fails with pages of
  # `undefined reference to 'log'/'exp'/'pow'/...` from naga's object files;
  # adding `-lpthread -lm` alone (no `-ldl` needed for that probe, since it
  # never called into the Vulkan-loading path) resolves every symbol and the
  # binary runs, successfully calling into the library (wgpuCreateInstance()
  # returns a real pointer). `-lgcc_s`/`-lstdc++` are not needed here for the
  # C++ target either: naturalPaint links with `c++`, which already carries
  # both, and no symbol from this archive comes from either -- checked by
  # confirming this same probe needed neither, and cross-checking that the
  # undefined-symbol set has no C++-runtime name in it (no `_Unwind_*`, no
  # `__cxa_*`, no libstdc++ demangled symbol) before ruling them out.
  target_link_libraries(wgpu_native INTERFACE dl pthread m)
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
