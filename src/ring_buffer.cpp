#include "frdx/ring_buffer.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace frdx {

std::size_t RingBuffer::round_pow2(std::size_t n) noexcept {
    if (n < 4096) n = 4096;
    constexpr auto kMaxPow2 = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (n > kMaxPow2) return kMaxPow2;
    return std::bit_ceil(n);
}

RingBuffer::RingBuffer(std::size_t capacity_bytes)
    : capacity_{round_pow2(capacity_bytes)}, mask_{capacity_ - 1},
      data_{std::make_unique<std::byte[]>(capacity_)} {}

std::size_t RingBuffer::readable() const noexcept {
    if (hold_.load(std::memory_order_acquire)) return 0;
    return write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire);
}

std::size_t RingBuffer::writable() const noexcept {
    return capacity_ -
           (write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire));
}

std::size_t RingBuffer::write(const void* src, std::size_t n) noexcept {
    // All-or-nothing: only write if the full payload fits.  write_partial()
    // commits bytes even on a partial write, so calling it and then returning 0
    // would violate the "never partial" contract by silently dropping data in
    // the ring while telling the caller nothing was written.
    if (n > writable()) return 0;
    return write_partial(src, n);
}

std::size_t RingBuffer::write_partial(const void* src, std::size_t n) noexcept {
    if (!src || n == 0) return 0;
    const auto w     = write_pos_.load(std::memory_order_relaxed);
    const auto r     = read_pos_.load(std::memory_order_acquire);
    const auto space = capacity_ - (w - r);
    if (space == 0) return 0;
    if (n > space) n = space;

    const auto off  = w & mask_;
    const auto tail = std::min(n, capacity_ - off);
    std::memcpy(data_.get() + off, src, tail);
    if (tail < n) std::memcpy(data_.get(), static_cast<const std::byte*>(src) + tail, n - tail);

    write_pos_.store(w + n, std::memory_order_release);
    return n;
}

std::size_t RingBuffer::read(void* dst, std::size_t n) noexcept {
    if (!dst || n == 0) return 0;
    if (hold_.load(std::memory_order_acquire)) return 0;

    std::scoped_lock lk{drain_mu_};
    const auto r     = read_pos_.load(std::memory_order_relaxed);
    const auto w     = write_pos_.load(std::memory_order_acquire);
    const auto avail = w - r;
    if (avail == 0) return 0;
    if (n > avail) n = avail;

    const auto off  = r & mask_;
    const auto tail = std::min(n, capacity_ - off);
    std::memcpy(dst, data_.get() + off, tail);
    if (tail < n) std::memcpy(static_cast<std::byte*>(dst) + tail, data_.get(), n - tail);

    read_pos_.store(r + n, std::memory_order_release);
    return n;
}

std::span<const std::byte> RingBuffer::readable_span(std::size_t n) const noexcept {
    if (n == 0) return {};
    if (hold_.load(std::memory_order_acquire)) return {};

    const auto r     = read_pos_.load(std::memory_order_relaxed);
    const auto w     = write_pos_.load(std::memory_order_acquire);
    const auto avail = w - r;
    if (avail == 0) return {};
    if (n > avail) n = avail;

    const auto off  = r & mask_;
    const auto tail = std::min(n, capacity_ - off);
    return {data_.get() + off, tail};
}

void RingBuffer::consume(std::size_t n) noexcept {
    if (n == 0) return;

    std::scoped_lock lk{drain_mu_};
    const auto r = read_pos_.load(std::memory_order_relaxed);
    const auto w = write_pos_.load(std::memory_order_acquire);
    const auto avail = w - r;
    if (n > avail) n = avail;
    read_pos_.store(r + n, std::memory_order_release);
}

void RingBuffer::drain() noexcept {
    std::scoped_lock lk{drain_mu_};
    read_pos_.store(write_pos_.load(std::memory_order_acquire), std::memory_order_release);
}

void RingBuffer::drop(std::size_t n) noexcept {
    std::scoped_lock lk{drain_mu_};
    const auto r     = read_pos_.load(std::memory_order_relaxed);
    const auto w     = write_pos_.load(std::memory_order_acquire);
    const auto avail = w - r;
    if (n > avail) n = avail;
    read_pos_.store(r + n, std::memory_order_release);
}

} // namespace frdx
