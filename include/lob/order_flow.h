#pragma once

#include <random>
#include <vector>

#include "lob/engine.h"

namespace lob {

struct FlowConfig {
  uint64_t seed = 1;
  Price start_mid = 10000;
  Price max_offset = 20;      // limit prices within this many ticks of the mid
  double limit_share = 0.60;  // the rest is split between cancels, markets and modifies
  double cancel_share = 0.25;
  double market_share = 0.08;
  Quantity max_quantity = 100;
};

// Synthetic order flow: a random-walk mid price with limit orders scattered
// around it, cancels of recent orders, occasional market orders and modifies.
// Deterministic for a given seed, so tests and benchmarks are repeatable.
class OrderFlow {
 public:
  explicit OrderFlow(FlowConfig config = {}) : cfg_(config), rng_(config.seed), mid_(config.start_mid) {}

  Command Next() {
    Command c;
    double r = Uniform();
    if (r < cfg_.limit_share || live_.empty()) {
      c.type = Command::Type::kNew;
      c.order_type = OrderType::kLimit;
      c.side = Uniform() < 0.5 ? Side::kBuy : Side::kSell;
      // Mostly passive, sometimes crossing the mid so trades happen.
      Price offset = static_cast<Price>(rng_() % (cfg_.max_offset + 1));
      bool aggressive = Uniform() < 0.25;
      Price delta = aggressive ? -offset / 4 : offset;
      c.price = c.side == Side::kBuy ? mid_ - delta : mid_ + delta;
      if (c.price < 1) c.price = 1;
      c.quantity = 1 + static_cast<Quantity>(rng_() % cfg_.max_quantity);
      c.id = next_id_++;
      live_.push_back(c.id);
    } else if (r < cfg_.limit_share + cfg_.cancel_share) {
      c.type = Command::Type::kCancel;
      c.id = TakeLive();
    } else if (r < cfg_.limit_share + cfg_.cancel_share + cfg_.market_share) {
      c.type = Command::Type::kNew;
      c.order_type = OrderType::kMarket;
      c.side = Uniform() < 0.5 ? Side::kBuy : Side::kSell;
      c.quantity = 1 + static_cast<Quantity>(rng_() % cfg_.max_quantity);
      c.id = next_id_++;
    } else {
      c.type = Command::Type::kModify;
      c.id = live_[rng_() % live_.size()];
      c.price = mid_ + static_cast<Price>(rng_() % (2 * cfg_.max_offset + 1)) - cfg_.max_offset;
      if (c.price < 1) c.price = 1;
      c.quantity = 1 + static_cast<Quantity>(rng_() % cfg_.max_quantity);
    }
    if (Uniform() < 0.05) mid_ += Uniform() < 0.5 ? -1 : 1;
    if (mid_ < cfg_.max_offset + 2) mid_ = cfg_.max_offset + 2;
    return c;
  }

  std::vector<Command> Generate(size_t n) {
    std::vector<Command> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(Next());
    return out;
  }

 private:
  double Uniform() { return std::uniform_real_distribution<double>(0, 1)(rng_); }

  // Cancels favour recent orders, like real flow; ids may already be filled.
  OrderId TakeLive() {
    size_t window = std::min<size_t>(live_.size(), 512);
    size_t i = live_.size() - 1 - rng_() % window;
    OrderId id = live_[i];
    live_[i] = live_.back();
    live_.pop_back();
    return id;
  }

  FlowConfig cfg_;
  std::mt19937_64 rng_;
  Price mid_;
  OrderId next_id_ = 1;
  std::vector<OrderId> live_;
};

}  // namespace lob
