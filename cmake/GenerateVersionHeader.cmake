# cmake/GenerateVersionHeader.cmake -- run at BUILD time (a custom command in
# src/CMakeLists.txt), not at configure time. Configure time would freeze the
# git hash at the last `cmake` invocation rather than the last build, so
# `naturalPaint --version` would go stale the moment someone commits without
# reconfiguring.
#
# Writes NP_OUT_FILE only when its content actually changes, so a build that
# does not touch the hash (most of them: `git describe` is cheap, but nothing
# downstream should pay for a header that reads the same as it did) never
# forces a relink of everything that includes it. See src/CMakeLists.txt's
# `np_version_header` target for the "run every build, rewrite rarely" pairing
# that makes that true under Ninja: a phony target that always re-runs this
# script, feeding a file whose mtime only changes when its bytes do.
#
# Expected -D arguments: NP_SOURCE_DIR, NP_VERSION, NP_OUT_FILE.

set(_np_hash "unknown")

find_program(_np_git_exe git)
if(_np_git_exe)
  execute_process(
    COMMAND "${_np_git_exe}" -C "${NP_SOURCE_DIR}" rev-parse --is-inside-work-tree
    RESULT_VARIABLE _np_in_repo
    OUTPUT_QUIET ERROR_QUIET)
  if(_np_in_repo EQUAL 0)
    execute_process(
      COMMAND "${_np_git_exe}" -C "${NP_SOURCE_DIR}" rev-parse --short=7 HEAD
      RESULT_VARIABLE _np_rev_result
      OUTPUT_VARIABLE _np_rev
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_np_rev_result EQUAL 0 AND NOT _np_rev STREQUAL "")
      set(_np_hash "${_np_rev}")
      # `diff-index --quiet HEAD --`: nonzero exit means the working tree or
      # the index differs from HEAD. Deliberately tracked changes only, the
      # same thing `git describe --dirty` checks -- an untracked scratch file
      # left in the tree does not make this build "dirty".
      execute_process(
        COMMAND "${_np_git_exe}" -C "${NP_SOURCE_DIR}" diff-index --quiet HEAD --
        RESULT_VARIABLE _np_dirty_result
        OUTPUT_QUIET ERROR_QUIET)
      if(NOT _np_dirty_result EQUAL 0)
        set(_np_hash "${_np_hash}-dirty")
      endif()
    endif()
  endif()
endif()

set(_np_content
"// Generated at build time by cmake/GenerateVersionHeader.cmake. Do not edit
// by hand -- it is rewritten on the next build. See src/app/Version.cpp for
// the function that reads these two constants.
#pragma once
namespace np {
inline constexpr const char* kVersionNumber = \"${NP_VERSION}\";
inline constexpr const char* kVersionGitHash = \"${_np_hash}\";
}
")

set(_np_write TRUE)
if(EXISTS "${NP_OUT_FILE}")
  file(READ "${NP_OUT_FILE}" _np_existing)
  if(_np_existing STREQUAL _np_content)
    set(_np_write FALSE)
  endif()
endif()

if(_np_write)
  get_filename_component(_np_out_dir "${NP_OUT_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${_np_out_dir}")
  file(WRITE "${NP_OUT_FILE}" "${_np_content}")
endif()
