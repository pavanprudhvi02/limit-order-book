#pragma once

#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/memory_pool.h"
#include "lob/types.h"

namespace lob {

struct Order;

// FIFO queue of resting orders at one price. Orders are linked intrusively
// (prev/next live inside Order), so adding at the tail and removing from the
// middle on cancel are both O(1) with no extra allocation.
struct PriceLevel {
  Price price = 0;
  Quantity total_quantity = 0;
  uint32_t order_count = 0;
  Order* head = nullptr;
  Order* tail = nullptr;
};

struct Order {
  OrderId id = 0;
  Side side = Side::kBuy;
  Price price = 0;
  Quantity quantity = 0;   // original size
  Quantity remaining = 0;  // still open
  Sequence sequence = 0;   // arrival order, for time priority
  PriceLevel* level = nullptr;
  Order* prev = nullptr;
  Order* next = nullptr;
};

struct Trade {
  OrderId aggressor_id;
  OrderId resting_id;
  Side aggressor_side;
  Price price;  // always the resting order's price
  Quantity quantity;
  Sequence sequence;
};

// Callbacks for everything the book does. All are optional.
struct BookListener {
  std::function<void(const Trade&)> on_trade;
  std::function<void(OrderId, Side, Price, Quantity remaining)> on_rest;  // order now resting on the book
  std::function<void(OrderId, Quantity cancelled)> on_cancel;           // cancelled, or unfilled part of IOC/market
  std::function<void(OrderId, RejectReason)> on_reject;
};

struct Level {
  Price price;
  Quantity quantity;
  uint32_t orders;
};

// Price-time priority limit order book for one instrument.
//
// - Bids live in a std::map ordered high to low, asks low to high, so the
//   best price on each side is begin(). Each map node holds a PriceLevel.
// - An unordered_map from order id to Order* gives O(1) cancel and modify.
// - Orders come from a pre-allocated MemoryPool, so the steady state makes
//   no allocations for orders.
//
// Matching walks the opposite side from the best price while prices cross,
// filling resting orders oldest first. Trades print at the resting price.
class OrderBook {
 public:
  explicit OrderBook(size_t max_orders = 1 << 20, BookListener listener = {});
  ~OrderBook();
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // Returns the quantity filled immediately. `price` is ignored for market orders.
  Quantity Submit(OrderId id, Side side, OrderType type, Price price, Quantity quantity);
  Quantity AddLimit(OrderId id, Side side, Price price, Quantity quantity) {
    return Submit(id, side, OrderType::kLimit, price, quantity);
  }
  Quantity AddMarket(OrderId id, Side side, Quantity quantity) { return Submit(id, side, OrderType::kMarket, 0, quantity); }

  bool Cancel(OrderId id);

  // Reducing the size at the same price keeps the order's place in the queue.
  // A new price, or a larger size, is a cancel-and-replace that goes to the back
  // (and may trade immediately if the new price crosses).
  bool Modify(OrderId id, Price new_price, Quantity new_quantity);

  std::optional<Price> BestBid() const;
  std::optional<Price> BestAsk() const;
  std::optional<Price> Spread() const;

  // Top `depth` levels of one side, best first.
  std::vector<Level> Depth(Side side, size_t depth) const;
  const Order* Find(OrderId id) const;

  size_t order_count() const { return orders_.size(); }
  size_t level_count(Side side) const { return side == Side::kBuy ? bids_.size() : asks_.size(); }
  uint64_t trade_count() const { return trade_count_; }
  const MemoryPool<Order>& pool() const { return pool_; }

  // Walks every level and order and checks the book's internal invariants
  // (sorted sides, uncrossed book, level totals, links). For tests.
  bool CheckInvariants() const;

 private:
  using Bids = std::map<Price, PriceLevel, std::greater<Price>>;
  using Asks = std::map<Price, PriceLevel, std::less<Price>>;

  template <typename Levels, typename Crosses>
  Quantity Match(Order& incoming, Levels& levels, Crosses crosses);
  template <typename Levels, typename Crosses>
  Quantity Available(const Levels& levels, Crosses crosses, Quantity wanted) const;
  void Rest(Order* order);
  void Unlink(Order* order);
  Quantity Remove(Order* order);  // takes a resting order off the book, returns its open quantity

  MemoryPool<Order> pool_;
  BookListener listener_;
  Bids bids_;
  Asks asks_;
  std::unordered_map<OrderId, Order*> orders_;
  Sequence next_sequence_ = 1;
  uint64_t trade_count_ = 0;
};

}  // namespace lob
