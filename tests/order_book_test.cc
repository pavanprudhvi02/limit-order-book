#include <gtest/gtest.h>

#include <vector>

#include "lob/order_book.h"

namespace lob {

// Records every callback so tests can assert on exactly what happened.
struct Recorder {
  std::vector<Trade> trades;
  std::vector<std::pair<OrderId, Quantity>> cancels;
  std::vector<std::pair<OrderId, RejectReason>> rejects;
  std::vector<OrderId> rests;

  BookListener Listener() {
    BookListener l;
    l.on_trade = [this](const Trade& t) { trades.push_back(t); };
    l.on_cancel = [this](OrderId id, Quantity q) { cancels.emplace_back(id, q); };
    l.on_reject = [this](OrderId id, RejectReason r) { rejects.emplace_back(id, r); };
    l.on_rest = [this](OrderId id, Side, Price, Quantity) { rests.push_back(id); };
    return l;
  }
};

class OrderBookTest : public ::testing::Test {
 protected:
  Recorder rec;
  OrderBook book{1024, rec.Listener()};

  void TearDown() override { EXPECT_TRUE(book.CheckInvariants()); }
};

TEST_F(OrderBookTest, RestsNonCrossingOrdersOnBothSides) {
  book.AddLimit(1, Side::kBuy, 100, 10);
  book.AddLimit(2, Side::kBuy, 99, 5);
  book.AddLimit(3, Side::kSell, 102, 7);
  EXPECT_EQ(book.BestBid(), 100);
  EXPECT_EQ(book.BestAsk(), 102);
  EXPECT_EQ(book.Spread(), 2);
  EXPECT_TRUE(rec.trades.empty());
  EXPECT_EQ(book.order_count(), 3u);
  auto bids = book.Depth(Side::kBuy, 5);
  ASSERT_EQ(bids.size(), 2u);
  EXPECT_EQ(bids[0].price, 100);
  EXPECT_EQ(bids[1].price, 99);
}

TEST_F(OrderBookTest, FillsAtTheRestingPrice) {
  book.AddLimit(1, Side::kSell, 101, 10);
  Quantity filled = book.AddLimit(2, Side::kBuy, 105, 4);  // willing to pay more
  EXPECT_EQ(filled, 4u);
  ASSERT_EQ(rec.trades.size(), 1u);
  EXPECT_EQ(rec.trades[0].price, 101);
  EXPECT_EQ(rec.trades[0].aggressor_id, 2u);
  EXPECT_EQ(rec.trades[0].resting_id, 1u);
  EXPECT_EQ(book.Find(1)->remaining, 6u);
  EXPECT_EQ(book.Find(2), nullptr);
}

TEST_F(OrderBookTest, OlderOrderAtTheSamePriceTradesFirst) {
  book.AddLimit(1, Side::kSell, 100, 5);
  book.AddLimit(2, Side::kSell, 100, 5);
  book.AddLimit(3, Side::kSell, 100, 5);
  book.AddLimit(4, Side::kBuy, 100, 7);
  ASSERT_EQ(rec.trades.size(), 2u);
  EXPECT_EQ(rec.trades[0].resting_id, 1u);
  EXPECT_EQ(rec.trades[0].quantity, 5u);
  EXPECT_EQ(rec.trades[1].resting_id, 2u);
  EXPECT_EQ(rec.trades[1].quantity, 2u);
  EXPECT_EQ(book.Find(2)->remaining, 3u);
  EXPECT_EQ(book.Find(3)->remaining, 5u);
}

TEST_F(OrderBookTest, BetterPriceBeatsEarlierTime) {
  book.AddLimit(1, Side::kBuy, 99, 5);
  book.AddLimit(2, Side::kBuy, 100, 5);  // later but better
  book.AddLimit(3, Side::kSell, 99, 5);
  ASSERT_EQ(rec.trades.size(), 1u);
  EXPECT_EQ(rec.trades[0].resting_id, 2u);
  EXPECT_EQ(rec.trades[0].price, 100);
}

TEST_F(OrderBookTest, SweepsSeveralLevelsAndRestsTheRemainder) {
  book.AddLimit(1, Side::kSell, 100, 3);
  book.AddLimit(2, Side::kSell, 101, 3);
  book.AddLimit(3, Side::kSell, 103, 3);
  Quantity filled = book.AddLimit(4, Side::kBuy, 101, 10);
  EXPECT_EQ(filled, 6u);
  ASSERT_EQ(rec.trades.size(), 2u);
  EXPECT_EQ(rec.trades[0].price, 100);
  EXPECT_EQ(rec.trades[1].price, 101);
  EXPECT_EQ(book.BestBid(), 101);  // remaining 4 rest at the limit
  EXPECT_EQ(book.Find(4)->remaining, 4u);
  EXPECT_EQ(book.BestAsk(), 103);
  EXPECT_EQ(book.level_count(Side::kSell), 1u);
}

TEST_F(OrderBookTest, MarketOrdersNeverRest) {
  book.AddLimit(1, Side::kBuy, 100, 5);
  book.AddLimit(2, Side::kBuy, 90, 5);
  Quantity filled = book.AddMarket(3, Side::kSell, 12);
  EXPECT_EQ(filled, 10u);
  EXPECT_EQ(rec.trades.size(), 2u);
  ASSERT_EQ(rec.cancels.size(), 1u);
  EXPECT_EQ(rec.cancels[0], std::make_pair(OrderId{3}, Quantity{2}));
  EXPECT_FALSE(book.BestBid().has_value());
  EXPECT_EQ(book.Find(3), nullptr);
}

TEST_F(OrderBookTest, ImmediateOrCancelFillsWhatItCanAtItsLimit) {
  book.AddLimit(1, Side::kSell, 100, 4);
  book.AddLimit(2, Side::kSell, 102, 4);
  Quantity filled = book.Submit(3, Side::kBuy, OrderType::kIoc, 101, 10);
  EXPECT_EQ(filled, 4u);
  EXPECT_EQ(rec.cancels.back(), std::make_pair(OrderId{3}, Quantity{6}));
  EXPECT_EQ(book.BestAsk(), 102);
  EXPECT_FALSE(book.BestBid().has_value());
}

TEST_F(OrderBookTest, FillOrKillIsAllOrNothing) {
  book.AddLimit(1, Side::kSell, 100, 4);
  book.AddLimit(2, Side::kSell, 101, 4);
  EXPECT_EQ(book.Submit(3, Side::kBuy, OrderType::kFok, 101, 9), 0u);
  EXPECT_EQ(rec.rejects.back(), std::make_pair(OrderId{3}, RejectReason::kFokNotFillable));
  EXPECT_TRUE(rec.trades.empty());
  EXPECT_EQ(book.Submit(4, Side::kBuy, OrderType::kFok, 101, 8), 8u);
  EXPECT_EQ(rec.trades.size(), 2u);
  EXPECT_FALSE(book.BestAsk().has_value());
}

TEST_F(OrderBookTest, CancelRemovesTheOrderAndEmptyLevels) {
  book.AddLimit(1, Side::kBuy, 100, 5);
  book.AddLimit(2, Side::kBuy, 100, 6);
  book.AddLimit(3, Side::kBuy, 100, 7);
  EXPECT_TRUE(book.Cancel(2));  // from the middle of the queue
  EXPECT_EQ(book.Depth(Side::kBuy, 1)[0].quantity, 12u);
  EXPECT_TRUE(book.Cancel(1));
  EXPECT_TRUE(book.Cancel(3));
  EXPECT_FALSE(book.BestBid().has_value());
  EXPECT_EQ(book.level_count(Side::kBuy), 0u);
  EXPECT_FALSE(book.Cancel(3));
  EXPECT_EQ(rec.rejects.back().second, RejectReason::kUnknownOrderId);
  EXPECT_EQ(book.pool().in_use(), 0u);
}

TEST_F(OrderBookTest, ShrinkingKeepsQueuePositionButRepricingLosesIt) {
  book.AddLimit(1, Side::kSell, 100, 10);
  book.AddLimit(2, Side::kSell, 100, 10);
  EXPECT_TRUE(book.Modify(1, 100, 4));  // size down: stays first
  book.AddLimit(3, Side::kBuy, 100, 4);
  ASSERT_EQ(rec.trades.size(), 1u);
  EXPECT_EQ(rec.trades[0].resting_id, 1u);

  book.AddLimit(4, Side::kSell, 100, 10);
  EXPECT_TRUE(book.Modify(2, 100, 20));  // size up: goes behind order 4
  book.AddLimit(5, Side::kBuy, 100, 10);
  EXPECT_EQ(rec.trades.back().resting_id, 4u);
}

TEST_F(OrderBookTest, ModifyToACrossingPriceTradesImmediately) {
  book.AddLimit(1, Side::kSell, 105, 5);
  book.AddLimit(2, Side::kBuy, 100, 5);
  EXPECT_TRUE(book.Modify(2, 105, 5));
  ASSERT_EQ(rec.trades.size(), 1u);
  EXPECT_EQ(rec.trades[0].aggressor_id, 2u);
  EXPECT_EQ(book.order_count(), 0u);
}

TEST_F(OrderBookTest, RejectsBadInput) {
  book.AddLimit(1, Side::kBuy, 100, 5);
  book.AddLimit(1, Side::kBuy, 100, 5);
  book.AddLimit(2, Side::kBuy, 100, 0);
  book.AddLimit(3, Side::kBuy, 0, 5);
  ASSERT_EQ(rec.rejects.size(), 3u);
  EXPECT_EQ(rec.rejects[0].second, RejectReason::kDuplicateOrderId);
  EXPECT_EQ(rec.rejects[1].second, RejectReason::kInvalidQuantity);
  EXPECT_EQ(rec.rejects[2].second, RejectReason::kInvalidPrice);
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBookPoolTest, RejectsTheRemainderWhenThePoolIsFull) {
  Recorder rec;
  OrderBook book(2, rec.Listener());
  book.AddLimit(1, Side::kBuy, 100, 1);
  book.AddLimit(2, Side::kBuy, 99, 1);
  book.AddLimit(3, Side::kBuy, 98, 1);
  EXPECT_EQ(rec.rejects.back(), std::make_pair(OrderId{3}, RejectReason::kPoolExhausted));
  EXPECT_EQ(book.order_count(), 2u);
  // Freed slots are reused.
  book.Cancel(1);
  book.AddLimit(4, Side::kBuy, 97, 1);
  EXPECT_EQ(book.order_count(), 2u);
  EXPECT_TRUE(book.CheckInvariants());
}

}  // namespace lob
