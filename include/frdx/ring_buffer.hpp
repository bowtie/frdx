#pragma once

// SPSC lock-free byte ring with power-of-2 capacity. Producer is the audio
// source's pump; consumer is FMOD's mixer thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace frdx {

class RingBuffer {
public:
    // capacity_bytes is rounded up to the next power of 2 (minimum 4096).
    explicit RingBuffer(std::size_t capacity_bytes);

    // Non-copyable (unique_ptr) and non-movable — moving would change
    // the addresses of the atomic read_pos_/write_pos_ that producer
    // and consumer threads rely on.
    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&)                 = delete;
    RingBuffer& operator=(RingBuffer&&)      = delete;

    std::size_t readable() const noexcept;
    std::size_t writable() const noexcept;

    // Returns bytes actually written; 0 if `n` doesn't fit (never partial).
    std::size_t write(const void* src, std::size_t n) noexcept;

    // Writes as much of `n` as fits into the available space; returns the
    // actual byte count written (may be less than `n`).  The remainder is
    // silently dropped — callers that need the full payload should use
    // write() and retry on 0, or size their ring to never overflow.
    std::size_t write_partial(const void* src, std::size_t n) noexcept;

    // Reads up to `n` bytes; returns the actual count read.
    std::size_t read(void* dst, std::size_t n) noexcept;

    // Returns the first contiguous readable span, capped at `n` bytes.
    std::span<const std::byte> readable_span(std::size_t n) const noexcept;

    // Advances the read cursor after consuming bytes from readable_span().
    void consume(std::size_t n) noexcept;

    // Discard everything currently in flight (advance read to write).
    void drain() noexcept;

private:
    static std::size_t round_pow2(std::size_t n) noexcept;

    std::size_t capacity_; // power of 2
    std::size_t mask_;     // capacity_ - 1
    std::unique_ptr<std::byte[]> data_;
    alignas(64) std::atomic<std::size_t> write_pos_{0};
    alignas(64) std::atomic<std::size_t> read_pos_{0};
};

} // namespace frdx
