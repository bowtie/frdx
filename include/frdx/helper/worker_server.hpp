#pragma once

#include <cstdint>
#include <string_view>

namespace frdx::helper {

// Entry point for worker mode. Runs the named-pipe server that handles
// spawn/kill/download/run operations. Returns when the parent process
// exits or a shutdown request is received.
int run_worker(std::string_view token, uint32_t parent_pid);

} // namespace frdx::helper
