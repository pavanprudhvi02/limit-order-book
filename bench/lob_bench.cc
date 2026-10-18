// lob_bench: measures the book on its own and the full threaded pipeline.
//
//   lob_bench [--ops N] [--seed S] [--rate CMDS_PER_SEC] [--skip-pipeline]
//
// 1. book:     replays N pre-generated commands straight into an OrderBook
//              on one thread and times each call.
// 2. pipeline: producer thread -> SPSC ring -> engine thread -> SPSC ring ->
//              consumer thread; latency is measured from when a command is
//              pushed to when each of its events is popped. The producer is
//              paced at --rate commands/s (default 100000) so the number is
//              the pipeline's latency, not time spent queued behind a flood;
//              --rate 0 sends as fast as possible to measure throughput.
//
// Numbers depend heavily on the machine; run with --ops large enough to
// get past warm-up, and pin threads to cores for stable results.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "lob/engine.h"
#include "lob/order_flow.h"

namespace {

using Clock = std::chrono::steady_clock;

uint64_t NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

void Report(const char* name, std::vector<uint64_t>& samples, double seconds, size_t ops) {
  std::sort(samples.begin(), samples.end());
  auto pct = [&](double p) { return samples[std::min(samples.size() - 1, static_cast<size_t>(p * samples.size()))]; };
  std::cout << std::left << std::setw(10) << name << std::right << std::setw(12)
            << static_cast<uint64_t>(ops / seconds) << " ops/s"
            << "   latency ns  p50 " << pct(0.50) << "  p90 " << pct(0.90) << "  p99 " << pct(0.99) << "  p99.9 "
            << pct(0.999) << "  max " << samples.back() << "\n";
}

void BenchBook(const std::vector<lob::Command>& commands) {
  lob::OrderBook book(1 << 20);
  std::vector<uint64_t> lat;
  lat.reserve(commands.size());
  auto start = Clock::now();
  for (const auto& c : commands) {
    uint64_t t0 = NowNs();
    switch (c.type) {
      case lob::Command::Type::kNew:
        book.Submit(c.id, c.side, c.order_type, c.price, c.quantity);
        break;
      case lob::Command::Type::kCancel:
        book.Cancel(c.id);
        break;
      case lob::Command::Type::kModify:
        book.Modify(c.id, c.price, c.quantity);
        break;
      case lob::Command::Type::kStop:
        break;
    }
    lat.push_back(NowNs() - t0);
  }
  double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  Report("book", lat, seconds, commands.size());
  std::cout << "          " << book.trade_count() << " trades, " << book.order_count() << " orders resting, "
            << book.level_count(lob::Side::kBuy) + book.level_count(lob::Side::kSell) << " price levels\n";
}

void BenchPipeline(const std::vector<lob::Command>& commands, uint64_t rate) {
  auto in = std::make_unique<lob::CommandRing>();
  auto out = std::make_unique<lob::EventRing>();
  lob::MatchingEngine engine(*in, *out, 1 << 20);
  engine.Start();

  std::atomic<bool> producer_done{false};
  auto start = Clock::now();
  std::thread producer([&] {
    const uint64_t gap_ns = rate ? 1'000'000'000ULL / rate : 0;
    uint64_t next_send = NowNs();
    for (auto c : commands) {
      if (gap_ns) {
        while (NowNs() < next_send) {
        }
        next_send += gap_ns;
      }
      c.sent_ns = NowNs();
      while (!in->TryPush(c)) {
      }
    }
    lob::Command stop;
    stop.type = lob::Command::Type::kStop;
    while (!in->TryPush(stop)) {
    }
    producer_done = true;
  });

  std::vector<uint64_t> lat;
  lat.reserve(commands.size() * 2);
  lob::Event e;
  for (;;) {
    if (out->TryPop(e)) {
      lat.push_back(NowNs() - e.sent_ns);
      continue;
    }
    if (producer_done && engine.commands_processed() == commands.size() && out->SizeApprox() == 0) break;
  }
  producer.join();
  engine.Wait();
  double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  Report(rate ? "pipeline" : "flood", lat, seconds, commands.size());
  std::cout << "          " << lat.size() << " events delivered\n";
}

}  // namespace

int main(int argc, char** argv) {
  size_t ops = 1'000'000;
  uint64_t seed = 1;
  bool pipeline = true;
  uint64_t rate = 100'000;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::stoull(argv[++i]);
    else if (!std::strcmp(argv[i], "--skip-pipeline")) pipeline = false;
  }

  lob::FlowConfig cfg;
  cfg.seed = seed;
  lob::OrderFlow flow(cfg);
  auto commands = flow.Generate(ops);
  std::cout << "generated " << ops << " commands (seed " << seed << ")\n";

  BenchBook(commands);
  if (pipeline) BenchPipeline(commands, rate);
  return 0;
}
