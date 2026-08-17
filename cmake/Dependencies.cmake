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
    "  unzip -o /tmp/wgpu.zip -d ${WGPU_DIR}")
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
