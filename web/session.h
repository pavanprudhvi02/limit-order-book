#pragma once

// Browser-facing wrapper around OrderBook. Every call returns a small JSON
// document (events produced plus the new top of book) so the JavaScript side
// never has to understand C++ types. Kept free of Emscripten headers so it
// can be compiled and tested natively.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lob/order_book.h"
#include "lob/order_flow.h"

namespace lob::web {

class Session {
 public:
  Session() { Reset(); }

  void Reset() {
    events_.clear();
    book_ = std::make_unique<OrderBook>(1 << 18, Listener());
    next_id_ = 1;
    sim_seed_ = 1;
  }

  // side: "BUY"/"SELL"; type: "LIMIT"/"MARKET"/"IOC"/"FOK".
  // Numbers arrive from JavaScript as doubles; ids and prices here are small integers.
  std::string Submit(const std::string& side, const std::string& type, double price, double quantity) {
    events_.clear();
    OrderType t = OrderType::kLimit;
    if (type == "MARKET") t = OrderType::kMarket;
    else if (type == "IOC") t = OrderType::kIoc;
    else if (type == "FOK") t = OrderType::kFok;
    OrderId id = next_id_++;
    book_->Submit(id, side == "SELL" ? Side::kSell : Side::kBuy, t, static_cast<Price>(price), ToQty(quantity));
    return Result(id);
  }

  std::string Cancel(double id) {
    events_.clear();
    book_->Cancel(static_cast<OrderId>(id));
    return Result(static_cast<OrderId>(id));
  }

  std::string Modify(double id, double price, double quantity) {
    events_.clear();
    book_->Modify(static_cast<OrderId>(id), static_cast<Price>(price), ToQty(quantity));
    return Result(static_cast<OrderId>(id));
  }

  // Pushes `count` synthetic orders through the book as fast as possible and
  // reports how long the matching took (events are counted, not returned).
  std::string Simulate(unsigned count) {
    FlowConfig cfg;
    cfg.seed = sim_seed_++;
    cfg.start_mid = Mid();
    OrderFlow flow(cfg);
    auto commands = flow.Generate(count);
    // Simulated ids live far above interactive ids so they never collide.
    const OrderId base = 1'000'000'000ULL * cfg.seed;

    counting_ = true;
    trades_ = 0;
    traded_qty_ = 0;
    auto start = std::chrono::steady_clock::now();
    for (const auto& c : commands) {
      switch (c.type) {
        case Command::Type::kNew:
          book_->Submit(base + c.id, c.side, c.order_type, c.price, c.quantity);
          break;
        case Command::Type::kCancel:
          book_->Cancel(base + c.id);
          break;
        case Command::Type::kModify:
          book_->Modify(base + c.id, c.price, c.quantity);
          break;
        case Command::Type::kStop:
          break;
      }
    }
    double micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    counting_ = false;
    events_.clear();

    std::string out = "{\"simulated\":" + std::to_string(count) + ",\"micros\":" + Num(micros) +
                      ",\"trades\":" + std::to_string(trades_) + ",\"tradedQty\":" + std::to_string(traded_qty_) +
                      ",\"book\":" + BookJson() + "}";
    return out;
  }

  std::string Snapshot() { return "{\"events\":[],\"book\":" + BookJson() + "}"; }

  bool Invariants() const { return book_->CheckInvariants(); }

 private:
  struct Ev {
    std::string json;
  };

  BookListener Listener() {
    BookListener l;
    l.on_trade = [this](const Trade& t) {
      if (counting_) {
        ++trades_;
        traded_qty_ += t.quantity;
        return;
      }
      events_.push_back({"{\"type\":\"trade\",\"aggressor\":" + std::to_string(t.aggressor_id) +
                         ",\"resting\":" + std::to_string(t.resting_id) + ",\"side\":\"" +
                         std::string(ToString(t.aggressor_side)) + "\",\"price\":" + std::to_string(t.price) +
                         ",\"qty\":" + std::to_string(t.quantity) + "}"});
    };
    l.on_rest = [this](OrderId id, Side side, Price price, Quantity qty) {
      if (counting_) return;
      events_.push_back({"{\"type\":\"rested\",\"id\":" + std::to_string(id) + ",\"side\":\"" +
                         std::string(ToString(side)) + "\",\"price\":" + std::to_string(price) +
                         ",\"qty\":" + std::to_string(qty) + "}"});
    };
    l.on_cancel = [this](OrderId id, Quantity qty) {
      if (counting_) return;
      events_.push_back({"{\"type\":\"cancelled\",\"id\":" + std::to_string(id) + ",\"qty\":" + std::to_string(qty) + "}"});
    };
    l.on_reject = [this](OrderId id, RejectReason r) {
      if (counting_) return;
      events_.push_back({"{\"type\":\"rejected\",\"id\":" + std::to_string(id) + ",\"reason\":\"" +
                         std::string(ToString(r)) + "\"}"});
    };
    return l;
  }

  static Quantity ToQty(double q) { return q <= 0 ? 0 : q > 4e9 ? 4000000000u : static_cast<Quantity>(q); }

  Price Mid() const {
    auto bid = book_->BestBid(), ask = book_->BestAsk();
    if (bid && ask) return (*bid + *ask) / 2;
    if (bid) return *bid;
    if (ask) return *ask;
    return 10000;
  }

  static std::string Num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
  }

  std::string Levels(Side side) const {
    std::string out = "[";
    bool first = true;
    for (const auto& l : book_->Depth(side, 12)) {
      if (!first) out += ",";
      first = false;
      out += "[" + std::to_string(l.price) + "," + std::to_string(l.quantity) + "," + std::to_string(l.orders) + "]";
    }
    return out + "]";
  }

  std::string BookJson() const {
    return "{\"bids\":" + Levels(Side::kBuy) + ",\"asks\":" + Levels(Side::kSell) +
           ",\"orders\":" + std::to_string(book_->order_count()) +
           ",\"poolInUse\":" + std::to_string(book_->pool().in_use()) +
           ",\"poolCapacity\":" + std::to_string(book_->pool().capacity()) +
           ",\"totalTrades\":" + std::to_string(book_->trade_count()) + "}";
  }

  std::string Result(OrderId id) const {
    std::string out = "{\"id\":" + std::to_string(id) + ",\"events\":[";
    for (size_t i = 0; i < events_.size(); ++i) {
      if (i) out += ",";
      out += events_[i].json;
    }
    return out + "],\"book\":" + BookJson() + "}";
  }

  std::unique_ptr<OrderBook> book_;
  std::vector<Ev> events_;
  OrderId next_id_ = 1;
  uint64_t sim_seed_ = 1;
  bool counting_ = false;
  uint64_t trades_ = 0;
  uint64_t traded_qty_ = 0;
};

}  // namespace lob::web
