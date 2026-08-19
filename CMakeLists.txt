cmake_minimum_required(VERSION 3.24)
project(naturalPaint LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Mixbox (Sochorova & Jamriska 2021) is CC BY-NC. Off by default; when OFF we fall
# back to a plain Kubelka-Munk two-constant mix with no licence encumbrance.
option(NP_USE_MIXBOX "Use the Mixbox pigment LUT (CC BY-NC, non-commercial only)" ON)

# OpenImageIO backs EXR/TIFF/HDR/DPX and flattened-PSD read (PLAN.md phase 4
# step 2). OFF by default, deliberately, and the reasoning is worth stating
# because it is the opposite of NP_USE_MIXBOX's:
#
#   * PRD I3 (P0) says format support is a runtime capability query, not a
#     build-time hard requirement. A default that makes `cmake -S . -B build`
#     fail on a machine without OpenImageIO would contradict that at the
#     first step -- so the core has to configure and build with no
#     OpenImageIO anywhere, and that has to be the path everyone gets for
#     free.
#   * PRD I1's PNG/JPEG/TGA/BMP are the "no optional dependency" set and are
#     stb-backed regardless of this option, so the default build is not a
#     crippled one; it is a complete implementation of I1.
#   * find_package(OpenImageIO) will not locate a non-standard install on its
#     own. This project's OpenImageIO is a deliberately stripped-down local
#     build (no LibRaw, no ffmpeg, no Python) under ~/.local/openimageio, so
#     turning the option ON is always accompanied by a CMAKE_PREFIX_PATH
#     anyway -- an ON default could only ever succeed by accident.
#
# Turn it on with:
#   cmake -S . -B build-oiio -DNP_USE_OIIO=ON \
#         -DCMAKE_PREFIX_PATH="$HOME/.local/openimageio"
#
# PLAN.md §1.5's lesson ("an unexercised build option is not a seam") applies
# with full force here: BOTH configurations are expected to build and pass
# --selftest, and --selftest's "format support" section deliberately asserts
# the *correct* answers for whichever backend set is compiled in rather than
# compiling itself out of the OFF build.
option(NP_USE_OIIO "Use OpenImageIO for EXR/TIFF/HDR/DPX and flattened-PSD read" OFF)

include(cmake/Dependencies.cmake)

add_subdirectory(src)
