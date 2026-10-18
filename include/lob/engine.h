#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "lob/order_book.h"
#include "lob/spsc_ring.h"

namespace lob {

// What a gateway thread sends to the engine.
struct Command {
  enum class Type : uint8_t { kNew, kCancel, kModify, kStop };
  Type type = Type::kNew;
  OrderType order_type = OrderType::kLimit;
  Side side = Side::kBuy;
  OrderId id = 0;
  Price price = 0;
  Quantity quantity = 0;
  uint64_t sent_ns = 0;  // set by the producer, carried onto every resulting event
};

// What the engine publishes. Plain data so it can be copied through a ring.
struct Event {
  enum class Type : uint8_t { kTrade, kRested, kCancelled, kRejected };
  Type type = Type::kTrade;
  Side side = Side::kBuy;
  RejectReason reason = RejectReason::kNone;
  OrderId id = 0;          // aggressor for trades
  OrderId resting_id = 0;  // trades only
  Price price = 0;
  Quantity quantity = 0;
  uint64_t sent_ns = 0;
};

inline constexpr size_t kRingSize = 1 << 16;
using CommandRing = SpscRing<Command, kRingSize>;
using EventRing = SpscRing<Event, kRingSize>;

// Runs an OrderBook on its own thread. Commands arrive on one SPSC ring
// (from a single gateway thread) and events leave on another (to a single
// consumer thread), so the hot path takes no locks. The engine busy-polls,
// trading a core for latency, as matching engines usually do.
class MatchingEngine {
 public:
  MatchingEngine(CommandRing& in, EventRing& out, size_t max_orders = 1 << 20);
  ~MatchingEngine();

  void Start();
  // Blocks until the engine thread has processed a kStop command.
  void Wait();

  const OrderBook& book() const { return book_; }
  uint64_t commands_processed() const { return processed_.load(std::memory_order_relaxed); }

 private:
  void Run();
  void Publish(const Event& e);

  CommandRing& in_;
  EventRing& out_;
  OrderBook book_;
  uint64_t current_sent_ns_ = 0;
  std::atomic<uint64_t> processed_{0};
  std::thread thread_;
};

}  // namespace lob
