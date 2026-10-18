#include "lob/order_book.h"

#include <algorithm>
#include <limits>

namespace lob {

OrderBook::OrderBook(size_t max_orders, BookListener listener) : pool_(max_orders), listener_(std::move(listener)) {
  orders_.reserve(max_orders);
}

OrderBook::~OrderBook() {
  for (auto& [_, order] : orders_) pool_.Free(order);
}

Quantity OrderBook::Submit(OrderId id, Side side, OrderType type, Price price, Quantity quantity) {
  auto reject = [&](RejectReason r) {
    if (listener_.on_reject) listener_.on_reject(id, r);
    return Quantity{0};
  };
  if (quantity == 0) return reject(RejectReason::kInvalidQuantity);
  if (type != OrderType::kMarket && price <= 0) return reject(RejectReason::kInvalidPrice);
  if (orders_.count(id)) return reject(RejectReason::kDuplicateOrderId);

  // A market order is a limit order at the most aggressive possible price.
  if (type == OrderType::kMarket) {
    price = side == Side::kBuy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
  }

  Order incoming;
  incoming.id = id;
  incoming.side = side;
  incoming.price = price;
  incoming.quantity = quantity;
  incoming.remaining = quantity;
  incoming.sequence = next_sequence_++;

  auto buy_crosses = [price](Price ask) { return ask <= price; };
  auto sell_crosses = [price](Price bid) { return bid >= price; };

  if (type == OrderType::kFok) {
    Quantity available = side == Side::kBuy ? Available(asks_, buy_crosses, quantity)
                                            : Available(bids_, sell_crosses, quantity);
    if (available < quantity) return reject(RejectReason::kFokNotFillable);
  }

  Quantity filled = side == Side::kBuy ? Match(incoming, asks_, buy_crosses) : Match(incoming, bids_, sell_crosses);

  if (incoming.remaining == 0) return filled;
  if (type != OrderType::kLimit) {
    if (listener_.on_cancel) listener_.on_cancel(id, incoming.remaining);
    return filled;
  }

  Order* resting = pool_.Allocate(incoming);
  if (resting == nullptr) {
    // Whatever traded already stands; only the remainder is refused.
    reject(RejectReason::kPoolExhausted);
    return filled;
  }
  Rest(resting);
  return filled;
}

template <typename Levels, typename Crosses>
Quantity OrderBook::Available(const Levels& levels, Crosses crosses, Quantity wanted) const {
  Quantity total = 0;
  for (const auto& [price, level] : levels) {
    if (!crosses(price)) break;
    total += level.total_quantity;
    if (total >= wanted) break;
  }
  return total;
}

template <typename Levels, typename Crosses>
Quantity OrderBook::Match(Order& incoming, Levels& levels, Crosses crosses) {
  Quantity filled = 0;
  while (incoming.remaining > 0 && !levels.empty()) {
    auto level_it = levels.begin();
    PriceLevel& level = level_it->second;
    if (!crosses(level.price)) break;

    // Oldest order at the best price trades first.
    while (incoming.remaining > 0 && level.head != nullptr) {
      Order* resting = level.head;
      Quantity qty = std::min(incoming.remaining, resting->remaining);
      incoming.remaining -= qty;
      resting->remaining -= qty;
      level.total_quantity -= qty;
      filled += qty;
      ++trade_count_;
      if (listener_.on_trade) {
        listener_.on_trade(Trade{incoming.id, resting->id, incoming.side, level.price, qty, incoming.sequence});
      }
      if (resting->remaining == 0) {
        Unlink(resting);
        orders_.erase(resting->id);
        pool_.Free(resting);
      }
    }
    if (level.head == nullptr) levels.erase(level_it);
  }
  return filled;
}

void OrderBook::Rest(Order* order) {
  PriceLevel* level;
  if (order->side == Side::kBuy) {
    auto [it, _] = bids_.try_emplace(order->price);
    level = &it->second;
  } else {
    auto [it, _] = asks_.try_emplace(order->price);
    level = &it->second;
  }
  level->price = order->price;
  order->level = level;
  order->prev = level->tail;
  order->next = nullptr;
  if (level->tail != nullptr) {
    level->tail->next = order;
  } else {
    level->head = order;
  }
  level->tail = order;
  level->total_quantity += order->remaining;
  ++level->order_count;
  orders_.emplace(order->id, order);
  if (listener_.on_rest) listener_.on_rest(order->id, order->side, order->price, order->remaining);
}

// Removes an order from its level's queue. The caller erases empty levels.
void OrderBook::Unlink(Order* order) {
  PriceLevel* level = order->level;
  if (order->prev != nullptr) {
    order->prev->next = order->next;
  } else {
    level->head = order->next;
  }
  if (order->next != nullptr) {
    order->next->prev = order->prev;
  } else {
    level->tail = order->prev;
  }
  level->total_quantity -= order->remaining;
  --level->order_count;
  order->prev = order->next = nullptr;
}

Quantity OrderBook::Remove(Order* order) {
  Quantity remaining = order->remaining;
  Side side = order->side;
  Price price = order->price;
  Unlink(order);
  if (order->level->head == nullptr) {
    if (side == Side::kBuy) {
      bids_.erase(price);
    } else {
      asks_.erase(price);
    }
  }
  orders_.erase(order->id);
  pool_.Free(order);
  return remaining;
}

bool OrderBook::Cancel(OrderId id) {
  auto it = orders_.find(id);
  if (it == orders_.end()) {
    if (listener_.on_reject) listener_.on_reject(id, RejectReason::kUnknownOrderId);
    return false;
  }
  Quantity remaining = Remove(it->second);
  if (listener_.on_cancel) listener_.on_cancel(id, remaining);
  return true;
}

bool OrderBook::Modify(OrderId id, Price new_price, Quantity new_quantity) {
  auto it = orders_.find(id);
  if (it == orders_.end()) {
    if (listener_.on_reject) listener_.on_reject(id, RejectReason::kUnknownOrderId);
    return false;
  }
  if (new_quantity == 0) return Cancel(id);
  Order* order = it->second;

  if (new_price == order->price && new_quantity <= order->remaining) {
    // Size down in place: keeps time priority.
    order->level->total_quantity -= order->remaining - new_quantity;
    order->remaining = new_quantity;
    return true;
  }

  // Anything else loses priority: remove and resubmit with the same id.
  // (A replace is not reported as a cancel.)
  Side side = order->side;
  Remove(order);
  Submit(id, side, OrderType::kLimit, new_price, new_quantity);
  return true;
}

std::optional<Price> OrderBook::BestBid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::BestAsk() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

std::optional<Price> OrderBook::Spread() const {
  auto bid = BestBid(), ask = BestAsk();
  if (!bid || !ask) return std::nullopt;
  return *ask - *bid;
}

std::vector<Level> OrderBook::Depth(Side side, size_t depth) const {
  std::vector<Level> out;
  auto collect = [&](const auto& levels) {
    for (const auto& [price, level] : levels) {
      if (out.size() == depth) break;
      out.push_back({price, level.total_quantity, level.order_count});
    }
  };
  if (side == Side::kBuy) {
    collect(bids_);
  } else {
    collect(asks_);
  }
  return out;
}

const Order* OrderBook::Find(OrderId id) const {
  auto it = orders_.find(id);
  return it == orders_.end() ? nullptr : it->second;
}

bool OrderBook::CheckInvariants() const {
  size_t counted = 0;
  auto check_side = [&](const auto& levels, Side side) {
    for (const auto& [price, level] : levels) {
      if (level.price != price || level.head == nullptr || level.head->prev != nullptr) return false;
      Quantity total = 0;
      uint32_t n = 0;
      Sequence last_seq = 0;
      const Order* prev = nullptr;
      for (const Order* o = level.head; o != nullptr; o = o->next) {
        if (o->side != side || o->price != price || o->level != &level || o->prev != prev) return false;
        if (o->remaining == 0 || o->remaining > o->quantity) return false;
        if (o->sequence <= last_seq) return false;  // FIFO by arrival
        auto it = orders_.find(o->id);
        if (it == orders_.end() || it->second != o || !pool_.Owns(o)) return false;
        last_seq = o->sequence;
        total += o->remaining;
        ++n;
        prev = o;
      }
      if (level.tail != prev || total != level.total_quantity || n != level.order_count) return false;
      counted += n;
    }
    return true;
  };
  if (!check_side(bids_, Side::kBuy) || !check_side(asks_, Side::kSell)) return false;
  if (counted != orders_.size() || counted != pool_.in_use()) return false;
  auto bid = BestBid(), ask = BestAsk();
  return !(bid && ask && *bid >= *ask);  // never crossed at rest
}

}  // namespace lob
