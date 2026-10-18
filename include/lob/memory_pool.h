#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace lob {

// Fixed-capacity object pool. All storage is allocated once up front; after
// that, Allocate and Free are O(1) pointer pops/pushes on an intrusive free
// list, with no calls into the system allocator on the hot path.
//
// Not thread-safe by design: the matching engine owns its pool and runs on
// one thread.
template <typename T>
class MemoryPool {
 public:
  explicit MemoryPool(size_t capacity) : capacity_(capacity) {
    slots_ = std::make_unique<Slot[]>(capacity);
    // Thread the free list through the slots in order, so early allocations
    // are contiguous in memory.
    for (size_t i = 0; i + 1 < capacity; ++i) slots_[i].next = &slots_[i + 1];
    if (capacity > 0) slots_[capacity - 1].next = nullptr;
    free_ = capacity > 0 ? &slots_[0] : nullptr;
  }

  ~MemoryPool() = default;  // T must be trivially destructible or released by the caller
  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  // Returns nullptr when the pool is exhausted.
  template <typename... Args>
  T* Allocate(Args&&... args) {
    if (free_ == nullptr) return nullptr;
    Slot* slot = free_;
    free_ = slot->next;
    ++in_use_;
    return new (slot->storage) T(std::forward<Args>(args)...);
  }

  void Free(T* object) {
    object->~T();
    auto* slot = reinterpret_cast<Slot*>(object);
    slot->next = free_;
    free_ = slot;
    --in_use_;
  }

  size_t capacity() const { return capacity_; }
  size_t in_use() const { return in_use_; }
  size_t available() const { return capacity_ - in_use_; }

  // True if `p` points into this pool's storage.
  bool Owns(const T* p) const {
    auto addr = reinterpret_cast<uintptr_t>(p);
    auto begin = reinterpret_cast<uintptr_t>(slots_.get());
    return addr >= begin && addr < begin + capacity_ * sizeof(Slot);
  }

 private:
  union Slot {
    Slot* next;
    alignas(T) unsigned char storage[sizeof(T)];
    Slot() : next(nullptr) {}
  };
  static_assert(offsetof(Slot, storage) == 0, "object must start at the slot address");

  size_t capacity_;
  std::unique_ptr<Slot[]> slots_;
  Slot* free_ = nullptr;
  size_t in_use_ = 0;
};

}  // namespace lob
