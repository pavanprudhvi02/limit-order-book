#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

using OrderId = uint64_t;
using Price = int64_t;     // integer ticks; never floating point
using Quantity = uint32_t;
using Sequence = uint64_t;

enum class Side : uint8_t { kBuy, kSell };

enum class OrderType : uint8_t {
  kLimit,  // match what crosses, rest the remainder
  kMarket, // match at any price, never rests
  kIoc,    // immediate-or-cancel: limit price, never rests
  kFok,    // fill-or-kill: fill completely at the limit price or do nothing
};

enum class RejectReason : uint8_t {
  kNone,
  kDuplicateOrderId,
  kUnknownOrderId,
  kInvalidQuantity,
  kInvalidPrice,
  kPoolExhausted,
  kFokNotFillable,
};

constexpr Side Opposite(Side s) { return s == Side::kBuy ? Side::kSell : Side::kBuy; }

constexpr std::string_view ToString(Side s) { return s == Side::kBuy ? "BUY" : "SELL"; }

constexpr std::string_view ToString(RejectReason r) {
  switch (r) {
    case RejectReason::kNone: return "none";
    case RejectReason::kDuplicateOrderId: return "duplicate order id";
    case RejectReason::kUnknownOrderId: return "unknown order id";
    case RejectReason::kInvalidQuantity: return "invalid quantity";
    case RejectReason::kInvalidPrice: return "invalid price";
    case RejectReason::kPoolExhausted: return "order pool exhausted";
    case RejectReason::kFokNotFillable: return "fill-or-kill not fillable";
  }
  return "?";
}

}  // namespace lob
