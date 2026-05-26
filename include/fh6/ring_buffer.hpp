#pragma once

// SPSC lock-free byte ring with power-of-2 capacity. Producer is the audio
// source's pump; consumer is FMOD's mixer thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fh6 {

class RingBuffer {
public:
    // capacity_bytes is rounded up to the next power of 2 (minimum 4096).
    explicit RingBuffer(std::size_t capacity_bytes);

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t readable() const noexcept;
    std::size_t writable() const noexcept;

    // Returns bytes actually written; 0 if `n` doesn't fit (never partial).
    std::size_t write(const void* src, std::size_t n) noexcept;

    // Reads up to `n` bytes; returns the actual count read.
    std::size_t read(void* dst, std::size_t n) noexcept;

    // Discard everything currently in flight (advance read to write).
    void drain() noexcept;

    // Hold mode: while engaged, reads return 0 even if there's data. Lets the
    // producer keep filling without anything reaching the mixer.
    void set_hold(bool on) noexcept { hold_.store(on, std::memory_order_release); }
    bool held() const noexcept { return hold_.load(std::memory_order_acquire); }

private:
    static std::size_t round_pow2(std::size_t n) noexcept;

    std::unique_ptr<std::byte[]> data_;
    std::size_t capacity_; // power of 2
    std::size_t mask_;     // capacity_ - 1
    alignas(64) std::atomic<std::size_t> write_pos_{0};
    alignas(64) std::atomic<std::size_t> read_pos_{0};
    alignas(64) std::atomic<bool> hold_{false};
};

} // namespace fh6
