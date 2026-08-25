cmake_minimum_required(VERSION 3.24)
project(naturalPaint LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Objective-C++, for the AppKit menu bar (src/ui/MacNativeMenu.mm). Deliberately
# NOT in the `project()` LANGUAGES list above: a language named there is probed
# on every platform, so a Linux or Windows configure would demand an
# Objective-C++ compiler it has no reason to own and fail over a file it was
# never going to build.
#
# It has to be *here* rather than beside the source it is for, in
# src/CMakeLists.txt, because `enable_language()` must be called from the
# highest directory common to every target using the language. Called from the
# subdirectory it raises no error at the call and then fails the generate step
# with `Missing variable is: CMAKE_OBJCXX_COMPILE_OBJECT` -- a message that
# blames the CMake installation rather than the placement. See the fuller note
# at src/CMakeLists.txt's native-menu-bar block.
if(APPLE)
  enable_language(OBJCXX)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Mixbox (Sochorova & Jamriska 2021) is CC BY-NC. Off by default; when OFF we fall
# back to a plain Kubelka-Munk two-constant mix with no licence encumbrance.
option(NP_USE_MIXBOX "Use the Mixbox pigment LUT (CC BY-NC, non-commercial only)" ON)

# OpenImageIO backs EXR/TIFF/HDR/DPX and flattened-PSD read (PLAN.md phase 4
# step 2). **It is a hard requirement, and it used to be an option that
# defaulted OFF.** The reversal is deliberate and the reason it was made is
# worth keeping, because the old argument was a good one that turned out to be
# answering the wrong question:
#
#   * The old default was justified from PRD I3 (P0) -- "format support is a
#     runtime capability query, not a build-time hard requirement" -- and that
#     reasoning is still correct **for foreign formats**. EXR, TIFF, HDR and
#     DPX genuinely are optional capabilities, and PRD I1's stb-backed
#     PNG/JPEG/TGA/BMP set genuinely does not need this dependency.
#   * What the argument missed is that `io/NpaintFile` builds the **native**
#     document format on multi-part EXR, so the same guard that made foreign
#     formats optional also made `saveNpaint()` and `loadNpaint()` refuse --
#     and `app/Journal`'s crash-recovery writer with them. The default build
#     could not save a document, could not open one, and had no recovery
#     journal, which is PRD O5-O10 (P0 data safety) absent from the
#     configuration everybody got by default.
#   * A foreign format you cannot read is a capability gap. A document you
#     cannot save is not an application. Those are not the same class of
#     compromise, and one option was deciding both.
#
# So the dependency is now required. `find_package(OpenImageIO REQUIRED)` in
# src/CMakeLists.txt is the enforcement, and a machine without OpenImageIO now
# fails at configure time with a message naming the fix rather than producing a
# binary that silently cannot persist work.
#
# This project's OpenImageIO is a deliberately stripped-down local build (no
# LibRaw, no ffmpeg, no Python) under ~/.local/openimageio, which find_package
# will not locate on its own, so configuring still needs:
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="$HOME/.local/openimageio"
#
# `NP_USE_OIIO` is still #defined for the compiler, unconditionally, so the
# ~230 `#if defined(NP_USE_OIIO)` sites across src/ remain true rather than
# becoming a lie. Their now-unreachable `#else` branches are dead code to be
# removed as its own change; folding that into this one would have mixed a
# decision with a 47-file sweep. PLAN.md 1.5's lesson ("an unexercised build
# option is not a seam") is exactly why the option is being removed rather
# than merely flipped to ON: an OFF path nobody builds is how NP_USE_MIXBOX
# became a documented fallback that does not exist.
if(DEFINED NP_USE_OIIO AND NOT NP_USE_OIIO)
  message(FATAL_ERROR
    "NP_USE_OIIO=OFF is no longer supported: OpenImageIO backs the native "
    ".npaint format and the recovery journal, so a build without it cannot "
    "save or open a document. Drop the flag and configure with "
    "-DCMAKE_PREFIX_PATH=\"$ENV{HOME}/.local/openimageio\" instead.")
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src)
