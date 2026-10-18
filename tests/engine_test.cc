#include <gtest/gtest.h>

#include <memory>
#include <thread>

#include "lob/engine.h"
#include "lob/order_flow.h"

namespace lob {

namespace {
bool SameEvent(const Event& a, const Event& b) {
  return a.type == b.type && a.id == b.id && a.resting_id == b.resting_id && a.price == b.price &&
         a.quantity == b.quantity && a.reason == b.reason;
}
}  // namespace

// Running the book on its own thread behind the rings must not change a
// single event compared with calling it directly.
TEST(MatchingEngineTest, ThreadedPipelineMatchesDirectCalls) {
  constexpr size_t kCommands = 200000;
  OrderFlow flow;
  auto commands = flow.Generate(kCommands);

  // Direct, single-threaded run.
  std::vector<Event> expected;
  {
    BookListener l;
    l.on_trade = [&](const Trade& t) {
      Event e;
      e.type = Event::Type::kTrade;
      e.id = t.aggressor_id;
      e.resting_id = t.resting_id;
      e.price = t.price;
      e.quantity = t.quantity;
      expected.push_back(e);
    };
    l.on_rest = [&](OrderId id, Side, Price p, Quantity q) {
      Event e;
      e.type = Event::Type::kRested;
      e.id = id;
      e.price = p;
      e.quantity = q;
      expected.push_back(e);
    };
    l.on_cancel = [&](OrderId id, Quantity q) {
      Event e;
      e.type = Event::Type::kCancelled;
      e.id = id;
      e.quantity = q;
      expected.push_back(e);
    };
    l.on_reject = [&](OrderId id, RejectReason r) {
      Event e;
      e.type = Event::Type::kRejected;
      e.id = id;
      e.reason = r;
      expected.push_back(e);
    };
    OrderBook book(1 << 18, l);
    for (const auto& c : commands) {
      if (c.type == Command::Type::kNew) book.Submit(c.id, c.side, c.order_type, c.price, c.quantity);
      if (c.type == Command::Type::kCancel) book.Cancel(c.id);
      if (c.type == Command::Type::kModify) book.Modify(c.id, c.price, c.quantity);
    }
  }

  // Producer thread -> engine thread -> this thread.
  auto in = std::make_unique<CommandRing>();
  auto out = std::make_unique<EventRing>();
  MatchingEngine engine(*in, *out, 1 << 18);
  engine.Start();
  std::thread producer([&] {
    for (const auto& c : commands) {
      while (!in->TryPush(c)) {
      }
    }
    Command stop;
    stop.type = Command::Type::kStop;
    while (!in->TryPush(stop)) {
    }
  });

  std::vector<Event> got;
  got.reserve(expected.size());
  Event e;
  while (got.size() < expected.size()) {
    if (out->TryPop(e)) got.push_back(e);
  }
  producer.join();
  engine.Wait();

  EXPECT_FALSE(out->TryPop(e)) << "engine produced more events than the direct run";
  EXPECT_EQ(engine.commands_processed(), kCommands);
  ASSERT_EQ(got.size(), expected.size());
  for (size_t i = 0; i < got.size(); ++i) ASSERT_TRUE(SameEvent(got[i], expected[i])) << "event " << i;
  EXPECT_TRUE(engine.book().CheckInvariants());
}

}  // namespace lob
