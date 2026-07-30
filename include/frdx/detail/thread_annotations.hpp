#pragma once

#include <mutex>

// ── Clang thread-safety annotations ───────────────────────────────────
// Enable compile-time data-race detection under clang.  No-ops on MSVC/GCC.
//
// Usage:
//   FRDX_GUARDED_BY(mu)        — member is protected by mutex 'mu'
//   FRDX_REQUIRES(mu)          — caller must hold 'mu'
//   FRDX_EXCLUDES(mu)          — caller must NOT hold 'mu' (function locks internally)
//   FRDX_NO_THREAD_SAFETY_ANALYSIS — suppress false positives on this function
//
// Use frdx::Mutex instead of std::mutex for any mutex referenced by
// thread-safety annotations (GUARDED_BY / REQUIRES / EXCLUDES).  clang-cl
// does not recognise the Windows STL's std::mutex as a capability type,
// so we wrap it with __attribute__((capability("mutex"))).
//
// Enable with clang-cl: -Wthread-safety

#if defined(__clang__)
#define FRDX_GUARDED_BY(x)          __attribute__((guarded_by(x)))
#define FRDX_REQUIRES(...)          __attribute__((exclusive_locks_required(__VA_ARGS__)))
#define FRDX_EXCLUDES(...)          __attribute__((locks_excluded(__VA_ARGS__)))
#define FRDX_NO_THREAD_SAFETY_ANALYSIS [[clang::no_thread_safety_analysis]]

// ── Capability type annotations (for custom mutex/lock types) ───────
#define FRDX_CAPABILITY(x)        __attribute__((capability(x)))
#define FRDX_SCOPED_CAPABILITY    __attribute__((scoped_lockable))
#define FRDX_ACQUIRE(...)         __attribute__((acquire_capability(__VA_ARGS__)))
#define FRDX_RELEASE(...)         __attribute__((release_capability(__VA_ARGS__)))
#else
#define FRDX_GUARDED_BY(x)
#define FRDX_REQUIRES(...)
#define FRDX_EXCLUDES(...)
#define FRDX_NO_THREAD_SAFETY_ANALYSIS

#define FRDX_CAPABILITY(x)
#define FRDX_SCOPED_CAPABILITY
#define FRDX_ACQUIRE(...)
#define FRDX_RELEASE(...)
#endif

namespace frdx {

// Drop-in replacement for std::mutex that the Clang thread-safety analyzer
// recognises as a capability.  Inherits lock() / unlock() / try_lock() so
// std::scoped_lock<Mutex>, std::unique_lock<Mutex>, and std::lock_guard<Mutex>
// all work transparently.
#if defined(__clang__)
struct FRDX_CAPABILITY("mutex") Mutex : std::mutex {};
#else
using Mutex = std::mutex;
#endif

// ── Annotated lock wrappers ──────────────────────────────────────────
// The Clang analyzer does not recognise std::scoped_lock / std::unique_lock
// acquiring custom capability types (only std::mutex from libc++).
// These wrappers are drop-in replacements that carry the right annotations.
// Use ScopedLock where std::scoped_lock<Mutex> was used and UniqueLock
// where std::unique_lock<Mutex> was used.

#if defined(__clang__)

class FRDX_SCOPED_CAPABILITY ScopedLock {
public:
    explicit ScopedLock(Mutex& m) FRDX_ACQUIRE(m) : m_(m) { m_.lock(); }
    ~ScopedLock() FRDX_RELEASE() { m_.unlock(); }

    ScopedLock(const ScopedLock&)            = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&&)                 = delete;
    ScopedLock& operator=(ScopedLock&&)      = delete;

private:
    Mutex& m_;
};

class FRDX_SCOPED_CAPABILITY UniqueLock {
public:
    explicit UniqueLock(Mutex& m) FRDX_ACQUIRE(m) : m_(&m), owns_(true) { m_->lock(); }
    ~UniqueLock() {
        if (owns_) m_->unlock();
    }

    UniqueLock(const UniqueLock&)            = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;
    UniqueLock(UniqueLock&& other) noexcept
        : m_(other.m_), owns_(other.owns_) {
        other.m_    = nullptr;
        other.owns_ = false;
    }
    UniqueLock& operator=(UniqueLock&& other) noexcept {
        if (this != &other) {
            if (owns_) m_->unlock();
            m_         = other.m_;
            owns_      = other.owns_;
            other.m_    = nullptr;
            other.owns_ = false;
        }
        return *this;
    }

    void unlock() FRDX_RELEASE() {
        if (owns_) {
            m_->unlock();
            owns_ = false;
        }
    }

private:
    Mutex* m_ = nullptr;
    bool owns_ = false;
};

#else
using ScopedLock = std::lock_guard<Mutex>;
using UniqueLock = std::unique_lock<Mutex>;
#endif

} // namespace frdx
