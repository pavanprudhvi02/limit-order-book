#include "lob/engine.h"

namespace lob {

namespace {

BookListener MakeListener(std::function<void(const Event&)> publish) {
  BookListener l;
  l.on_trade = [publish](const Trade& t) {
    Event e;
    e.type = Event::Type::kTrade;
    e.side = t.aggressor_side;
    e.id = t.aggressor_id;
    e.resting_id = t.resting_id;
    e.price = t.price;
    e.quantity = t.quantity;
    publish(e);
  };
  l.on_rest = [publish](OrderId id, Side side, Price price, Quantity remaining) {
    Event e;
    e.type = Event::Type::kRested;
    e.id = id;
    e.side = side;
    e.price = price;
    e.quantity = remaining;
    publish(e);
  };
  l.on_cancel = [publish](OrderId id, Quantity qty) {
    Event e;
    e.type = Event::Type::kCancelled;
    e.id = id;
    e.quantity = qty;
    publish(e);
  };
  l.on_reject = [publish](OrderId id, RejectReason reason) {
    Event e;
    e.type = Event::Type::kRejected;
    e.id = id;
    e.reason = reason;
    publish(e);
  };
  return l;
}

}  // namespace

MatchingEngine::MatchingEngine(CommandRing& in, EventRing& out, size_t max_orders)
    : in_(in), out_(out), book_(max_orders, MakeListener([this](const Event& e) { Publish(e); })) {}

MatchingEngine::~MatchingEngine() {
  if (thread_.joinable()) thread_.join();
}

void MatchingEngine::Start() { thread_ = std::thread([this] { Run(); }); }

void MatchingEngine::Wait() {
  if (thread_.joinable()) thread_.join();
}

void MatchingEngine::Publish(const Event& e) {
  Event out = e;
  out.sent_ns = current_sent_ns_;
  // If the consumer falls behind, wait for it rather than drop events.
  while (!out_.TryPush(out)) {
  }
}

void MatchingEngine::Run() {
  Command cmd;
  for (;;) {
    if (!in_.TryPop(cmd)) continue;  // busy-poll
    current_sent_ns_ = cmd.sent_ns;
    switch (cmd.type) {
      case Command::Type::kNew:
        book_.Submit(cmd.id, cmd.side, cmd.order_type, cmd.price, cmd.quantity);
        break;
      case Command::Type::kCancel:
        book_.Cancel(cmd.id);
        break;
      case Command::Type::kModify:
        book_.Modify(cmd.id, cmd.price, cmd.quantity);
        break;
      case Command::Type::kStop:
        return;
    }
    processed_.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace lob
