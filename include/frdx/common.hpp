#pragma once

// ── Centralized C++ standard library includes ──────────────────────────
// Every TU in the project needs at least some of these; pulling them in
// through a single header avoids N different ad-hoc inclusion patterns
// and makes it easy to audit which std types the codebase depends on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
