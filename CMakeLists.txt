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

include(cmake/Dependencies.cmake)

add_subdirectory(src)
