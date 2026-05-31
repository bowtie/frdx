#pragma once

// Internal header shared across Forza Radio DX source files.
// Defines RAII Win32 handle wrappers using Microsoft WIL.
// Include this instead of duplicating the typedefs in each .cpp.

#include <windows.h>
#include <winhttp.h>
#include <wil/resource.h>

#include <cstdio> // std::FILE, std::fclose

namespace frdx::detail {

// Generic Win32 HANDLE, auto-closed via CloseHandle.
// Used for pipes, files, jobs, processes, threads, etc.
using unique_handle = wil::unique_handle;

// VirtualAlloc-allocated memory, auto-freed via VirtualFree(MEM_RELEASE).
// Used for temporary allocations (e.g. swapping buffers into foreign heaps)
// that must be freed through VirtualFree rather than the C++ free().
inline void virtual_free(void* p) noexcept {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

using unique_virtual = wil::unique_any<void*, decltype(&virtual_free), virtual_free>;

// std::FILE handle, auto-closed via std::fclose.
// Used for log files and other C stdio file handles.
inline void file_close(std::FILE* f) noexcept {
    if (f) std::fclose(f);
}

using unique_file = wil::unique_any<std::FILE*, decltype(&file_close), file_close>;

} // namespace frdx::detail