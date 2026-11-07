# FetchContent PATCH_COMMAND for mpg123 (runs in the mpg123 source dir).
#
# Upstream bf41246 ("compat: a bit more careful fallbacks for STD*_FILENO",
# svn r5635) left a bare `#define` after the _WIN32 STD*_FILENO block in
# src/compat/compat.h.  Toolchains without <unistd.h> (clang-cl, MSVC) take
# that branch and fail with "macro name missing".  Drop the stray line.
#
# Idempotent: FetchContent re-runs the patch step on updates.  Once upstream
# fixes the line, this reports it and the script can be deleted.

set(_file "src/compat/compat.h")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "patch_mpg123: ${_file} not found (run from mpg123 source dir)")
endif()

file(READ "${_file}" _src)
string(REGEX REPLACE "(_fileno\\(stderr\\)[ \t]*\r?\n)#define[ \t]*\r?\n" "\\1" _patched "${_src}")

if(_patched STREQUAL _src)
    message(STATUS "patch_mpg123: no stray #define in ${_file}; patch no longer needed")
else()
    file(WRITE "${_file}" "${_patched}")
    message(STATUS "patch_mpg123: removed stray #define from ${_file}")
endif()
