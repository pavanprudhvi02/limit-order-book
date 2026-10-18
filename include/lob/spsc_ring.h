#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>

namespace lob {

// 64 bytes on x86-64 and most ARM cores. Kept as a constant rather than
// std::hardware_destructive_interference_size, whose value can change with
// compiler flags and would silently change the struct layout.
inline constexpr size_t kCacheLine = 64;

// Lock-free single-producer / single-consumer ring buffer.
//
// The producer only writes `tail_`, the consumer only writes `head_`, so no
// read-modify-write atomics are needed: a release store publishes a slot and
// the matching acquire load on the other side makes its contents visible.
// Each side also keeps a cached copy of the other side's index and only
// reloads it when the ring looks full (or empty), which keeps the shared
// cache lines from bouncing between cores on every operation.
//
// Capacity must be a power of two; one slot is never used, to tell full from empty.
template <typename T, size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_nothrow_copy_assignable_v<T> || std::is_nothrow_move_assignable_v<T>);

 public:
  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer side. Returns false if the ring is full.
  bool TryPush(const T& value) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t next = (tail + 1) & kMask;
    if (next == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (next == cached_head_) return false;
    }
    buffer_[tail] = value;
    tail_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false if the ring is empty.
  bool TryPop(T& out) {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) return false;
    }
    out = std::move(buffer_[head]);
    head_.store((head + 1) & kMask, std::memory_order_release);
    return true;
  }

  // Approximate when called concurrently; exact when quiescent.
  size_t SizeApprox() const {
    size_t head = head_.load(std::memory_order_acquire);
    size_t tail = tail_.load(std::memory_order_acquire);
    return (tail - head) & kMask;
  }

  static constexpr size_t capacity() { return Capacity - 1; }

 private:
  static constexpr size_t kMask = Capacity - 1;

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<size_t> head_{0};
  size_t cached_tail_ = 0;
  // Producer-owned line.
  alignas(kCacheLine) std::atomic<size_t> tail_{0};
  size_t cached_head_ = 0;
  // Slots on their own lines, away from the indices.
  alignas(kCacheLine) T buffer_[Capacity];
};

}  // namespace lob
