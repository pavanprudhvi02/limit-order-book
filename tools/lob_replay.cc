// lob_replay: feeds a CSV of orders through the book and prints every event
// and the final depth. Handy for walking through matching by hand.
//
//   lob_replay data/sample_orders.csv
//
// Lines (blank lines and '#' comments are skipped):
//   N,<id>,<BUY|SELL>,<LIMIT|MARKET|IOC|FOK>,<price>,<qty>   new order
//   C,<id>                                                   cancel
//   M,<id>,<price>,<qty>                                     modify

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lob/order_book.h"

namespace {

std::vector<std::string> SplitCsv(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string field;
  while (std::getline(ss, field, ',')) {
    field.erase(0, field.find_first_not_of(" \t\r"));
    field.erase(field.find_last_not_of(" \t\r") + 1);
    out.push_back(field);
  }
  return out;
}

bool ParseType(const std::string& s, lob::OrderType* t) {
  if (s == "LIMIT") *t = lob::OrderType::kLimit;
  else if (s == "MARKET") *t = lob::OrderType::kMarket;
  else if (s == "IOC") *t = lob::OrderType::kIoc;
  else if (s == "FOK") *t = lob::OrderType::kFok;
  else return false;
  return true;
}

void PrintDepth(const lob::OrderBook& book) {
  auto asks = book.Depth(lob::Side::kSell, 10);
  auto bids = book.Depth(lob::Side::kBuy, 10);
  std::cout << "\n        price     qty  orders\n";
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
    std::cout << "  ASK " << std::setw(8) << it->price << std::setw(8) << it->quantity << std::setw(8) << it->orders << "\n";
  }
  std::cout << "  -------------------------------\n";
  for (const auto& l : bids) {
    std::cout << "  BID " << std::setw(8) << l.price << std::setw(8) << l.quantity << std::setw(8) << l.orders << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: lob_replay FILE.csv\n";
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::cerr << "cannot open " << argv[1] << "\n";
    return 1;
  }

  lob::BookListener l;
  l.on_trade = [](const lob::Trade& t) {
    std::cout << "  TRADE   " << t.quantity << " @ " << t.price << "  (aggressor " << t.aggressor_id << " "
              << lob::ToString(t.aggressor_side) << ", resting " << t.resting_id << ")\n";
  };
  l.on_rest = [](lob::OrderId id, lob::Side side, lob::Price price, lob::Quantity qty) {
    std::cout << "  RESTED  order " << id << " " << lob::ToString(side) << " " << qty << " @ " << price << "\n";
  };
  l.on_cancel = [](lob::OrderId id, lob::Quantity qty) {
    std::cout << "  CANCEL  order " << id << " (" << qty << " unfilled)\n";
  };
  l.on_reject = [](lob::OrderId id, lob::RejectReason r) {
    std::cout << "  REJECT  order " << id << ": " << lob::ToString(r) << "\n";
  };
  lob::OrderBook book(1 << 16, l);

  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    auto f = SplitCsv(line);
    try {
      if (f[0] == "N" && f.size() == 6) {
        lob::OrderType type;
        if (!ParseType(f[3], &type) || (f[2] != "BUY" && f[2] != "SELL")) throw std::invalid_argument("bad field");
        std::cout << "> " << line << "\n";
        book.Submit(std::stoull(f[1]), f[2] == "BUY" ? lob::Side::kBuy : lob::Side::kSell, type, std::stoll(f[4]),
                    static_cast<lob::Quantity>(std::stoul(f[5])));
      } else if (f[0] == "C" && f.size() == 2) {
        std::cout << "> " << line << "\n";
        book.Cancel(std::stoull(f[1]));
      } else if (f[0] == "M" && f.size() == 4) {
        std::cout << "> " << line << "\n";
        book.Modify(std::stoull(f[1]), std::stoll(f[2]), static_cast<lob::Quantity>(std::stoul(f[3])));
      } else {
        throw std::invalid_argument("unknown record");
      }
    } catch (const std::exception&) {
      std::cerr << "line " << line_no << ": cannot parse '" << line << "'\n";
      return 1;
    }
  }
  PrintDepth(book);
  return 0;
}
