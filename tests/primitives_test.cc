#include <gtest/gtest.h>

#include <set>
#include <thread>

#include "lob/memory_pool.h"
#include "lob/spsc_ring.h"

namespace lob {

struct Widget {
  int a;
  double b;
  Widget(int x, double y) : a(x), b(y) {}
};

TEST(MemoryPoolTest, AllocatesUpToCapacityThenReusesFreedSlots) {
  MemoryPool<Widget> pool(3);
  Widget* w1 = pool.Allocate(1, 1.0);
  Widget* w2 = pool.Allocate(2, 2.0);
  Widget* w3 = pool.Allocate(3, 3.0);
  ASSERT_NE(w3, nullptr);
  EXPECT_EQ(pool.Allocate(4, 4.0), nullptr);
  EXPECT_EQ(pool.in_use(), 3u);
  EXPECT_EQ(w2->a, 2);
  EXPECT_TRUE(pool.Owns(w1));

  pool.Free(w2);
  Widget* w4 = pool.Allocate(4, 4.0);
  EXPECT_EQ(w4, w2) << "the most recently freed slot is reused first";
  EXPECT_EQ(w4->a, 4);
  pool.Free(w1);
  pool.Free(w3);
  pool.Free(w4);
  EXPECT_EQ(pool.available(), 3u);
}

TEST(MemoryPoolTest, HandsOutDistinctAlignedSlots) {
  MemoryPool<Widget> pool(1000);
  std::set<Widget*> seen;
  for (int i = 0; i < 1000; ++i) {
    Widget* w = pool.Allocate(i, 0.0);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(w) % alignof(Widget), 0u);
    EXPECT_TRUE(seen.insert(w).second);
  }
  Widget outside(0, 0);
  EXPECT_FALSE(pool.Owns(&outside));
  for (Widget* w : seen) pool.Free(w);
}

TEST(SpscRingTest, FifoAndFullEmptyBehaviour) {
  SpscRing<int, 8> ring;
  EXPECT_EQ(ring.capacity(), 7u);
  int out;
  EXPECT_FALSE(ring.TryPop(out));
  for (int i = 0; i < 7; ++i) EXPECT_TRUE(ring.TryPush(i));
  EXPECT_FALSE(ring.TryPush(99)) << "one slot stays empty to tell full from empty";
  EXPECT_EQ(ring.SizeApprox(), 7u);
  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(ring.TryPop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(ring.TryPop(out));
  // Wrap around several times.
  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(ring.TryPush(round * 10 + i));
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(ring.TryPop(out));
      EXPECT_EQ(out, round * 10 + i);
    }
  }
}

TEST(SpscRingTest, TwoThreadsTransferEveryItemInOrder) {
  constexpr uint64_t kItems = 2'000'000;
  SpscRing<uint64_t, 1024> ring;
  std::thread producer([&] {
    for (uint64_t i = 1; i <= kItems; ++i) {
      while (!ring.TryPush(i)) {
      }
    }
  });
  uint64_t expected = 1, sum = 0, value;
  while (expected <= kItems) {
    if (ring.TryPop(value)) {
      ASSERT_EQ(value, expected);
      sum += value;
      ++expected;
    }
  }
  producer.join();
  EXPECT_EQ(sum, kItems * (kItems + 1) / 2);
}

}  // namespace lob
