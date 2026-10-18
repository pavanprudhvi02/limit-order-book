// Differential test: the optimized book must produce exactly the same trades,
// cancels, rejects and final book as a deliberately simple reference
// implementation (linear scans over a vector) on long random order flows.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

#include "lob/order_book.h"
#include "lob/order_flow.h"

namespace lob {
namespace {

class ReferenceBook {
 public:
  struct O {
    OrderId id;
    Side side;
    Price price;
    Quantity remaining;
    uint64_t seq;
  };
  std::vector<std::string> log;

  void Submit(OrderId id, Side side, OrderType type, Price price, Quantity qty) {
    if (qty == 0) return Reject(id, RejectReason::kInvalidQuantity);
    if (type != OrderType::kMarket && price <= 0) return Reject(id, RejectReason::kInvalidPrice);
    if (FindIdx(id)) return Reject(id, RejectReason::kDuplicateOrderId);
    if (type == OrderType::kMarket) {
      price = side == Side::kBuy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
    }
    auto crosses = [&](const O& r) {
      return r.side != side && (side == Side::kBuy ? r.price <= price : r.price >= price);
    };
    if (type == OrderType::kFok) {
      Quantity avail = 0;
      for (const auto& r : book_) {
        if (crosses(r)) avail += r.remaining;
      }
      if (avail < qty) return Reject(id, RejectReason::kFokNotFillable);
    }
    Quantity left = qty;
    while (left > 0) {
      // Best opposite order: best price, then oldest.
      int best = -1;
      for (int i = 0; i < static_cast<int>(book_.size()); ++i) {
        if (!crosses(book_[i])) continue;
        if (best < 0) {
          best = i;
          continue;
        }
        const O& a = book_[i];
        const O& b = book_[best];
        bool better = side == Side::kBuy ? a.price < b.price : a.price > b.price;
        if (better || (a.price == b.price && a.seq < b.seq)) best = i;
      }
      if (best < 0) break;
      O& r = book_[best];
      Quantity q = std::min(left, r.remaining);
      std::ostringstream s;
      s << "T " << id << " " << r.id << " " << r.price << " " << q;
      log.push_back(s.str());
      left -= q;
      r.remaining -= q;
      if (r.remaining == 0) book_.erase(book_.begin() + best);
    }
    if (left == 0) return;
    if (type != OrderType::kLimit) {
      log.push_back("C " + std::to_string(id) + " " + std::to_string(left));
      return;
    }
    book_.push_back({id, side, price, left, ++seq_});
  }

  void Cancel(OrderId id) {
    auto idx = FindIdx(id);
    if (!idx) return Reject(id, RejectReason::kUnknownOrderId);
    log.push_back("C " + std::to_string(id) + " " + std::to_string(book_[*idx].remaining));
    book_.erase(book_.begin() + static_cast<long>(*idx));
  }

  void Modify(OrderId id, Price price, Quantity qty) {
    auto idx = FindIdx(id);
    if (!idx) return Reject(id, RejectReason::kUnknownOrderId);
    if (qty == 0) return Cancel(id);
    O& o = book_[*idx];
    if (price == o.price && qty <= o.remaining) {
      o.remaining = qty;
      return;
    }
    Side side = o.side;
    book_.erase(book_.begin() + static_cast<long>(*idx));
    Submit(id, side, OrderType::kLimit, price, qty);
  }

  // price -> (total quantity, order count) per side.
  std::map<Price, std::pair<Quantity, uint32_t>> Levels(Side side) const {
    std::map<Price, std::pair<Quantity, uint32_t>> out;
    for (const auto& o : book_) {
      if (o.side != side) continue;
      out[o.price].first += o.remaining;
      out[o.price].second += 1;
    }
    return out;
  }

 private:
  std::optional<size_t> FindIdx(OrderId id) const {
    for (size_t i = 0; i < book_.size(); ++i) {
      if (book_[i].id == id) return i;
    }
    return std::nullopt;
  }
  void Reject(OrderId id, RejectReason r) {
    log.push_back("R " + std::to_string(id) + " " + std::string(ToString(r)));
  }

  std::vector<O> book_;
  uint64_t seq_ = 0;
};

void RunAgainstReference(uint64_t seed, size_t ops) {
  std::vector<std::string> log;
  BookListener l;
  l.on_trade = [&](const Trade& t) {
    std::ostringstream s;
    s << "T " << t.aggressor_id << " " << t.resting_id << " " << t.price << " " << t.quantity;
    log.push_back(s.str());
  };
  l.on_cancel = [&](OrderId id, Quantity q) { log.push_back("C " + std::to_string(id) + " " + std::to_string(q)); };
  l.on_reject = [&](OrderId id, RejectReason r) {
    log.push_back("R " + std::to_string(id) + " " + std::string(ToString(r)));
  };
  OrderBook book(1 << 16, l);
  ReferenceBook ref;

  FlowConfig cfg;
  cfg.seed = seed;
  OrderFlow flow(cfg);
  std::mt19937 rng(static_cast<uint32_t>(seed));
  for (size_t i = 0; i < ops; ++i) {
    Command c = flow.Next();
    // Mix in IOC and FOK orders, which the generator does not produce on its own.
    if (c.type == Command::Type::kNew && c.order_type == OrderType::kLimit && rng() % 10 == 0) {
      c.order_type = rng() % 2 ? OrderType::kIoc : OrderType::kFok;
    }
    switch (c.type) {
      case Command::Type::kNew:
        book.Submit(c.id, c.side, c.order_type, c.price, c.quantity);
        ref.Submit(c.id, c.side, c.order_type, c.price, c.quantity);
        break;
      case Command::Type::kCancel:
        book.Cancel(c.id);
        ref.Cancel(c.id);
        break;
      case Command::Type::kModify:
        book.Modify(c.id, c.price, c.quantity);
        ref.Modify(c.id, c.price, c.quantity);
        break;
      case Command::Type::kStop:
        break;
    }
    ASSERT_EQ(log.size(), ref.log.size()) << "event count differs after op " << i;
    if (i % 1000 == 0) {
      ASSERT_TRUE(book.CheckInvariants()) << "after op " << i;
    }
  }
  ASSERT_EQ(log, ref.log);
  ASSERT_TRUE(book.CheckInvariants());

  for (Side side : {Side::kBuy, Side::kSell}) {
    auto expected = ref.Levels(side);
    auto depth = book.Depth(side, expected.size() + 1);
    ASSERT_EQ(depth.size(), expected.size());
    for (const auto& lvl : depth) {
      auto it = expected.find(lvl.price);
      ASSERT_NE(it, expected.end()) << "unexpected level " << lvl.price;
      EXPECT_EQ(lvl.quantity, it->second.first);
      EXPECT_EQ(lvl.orders, it->second.second);
    }
  }
  EXPECT_GT(book.trade_count(), ops / 20) << "flow should produce plenty of trades";
}

}  // namespace

class ReferenceTest : public ::testing::TestWithParam<uint64_t> {};

TEST_P(ReferenceTest, MatchesTheReferenceImplementation) { RunAgainstReference(GetParam(), 30000); }

INSTANTIATE_TEST_SUITE_P(Seeds, ReferenceTest, ::testing::Values(1, 2, 3, 42, 2025));

}  // namespace lob
