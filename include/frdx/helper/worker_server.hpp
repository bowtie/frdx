#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <windows.h>

namespace frdx::helper {

// Entry point for standalone worker mode. Runs the named-pipe server
// that handles kill/download/run operations. Returns when the
// parent process exits or a shutdown request is received.
int run_worker(std::string_view token, uint32_t parent_pid);

// Runs the worker accept loop on a given pipe name, shutting down when
// the provided event is signaled.  Designed to run in a thread inside
// the unified external helper.  Returns when shutdown_event is signaled.
void run_worker_loop(const std::wstring& pipe_name, HANDLE shutdown_event,
                     std::string_view token);

} // namespace frdx::helper
