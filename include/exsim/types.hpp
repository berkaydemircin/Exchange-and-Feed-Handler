#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace exsim {

using SeqNo = std::uint64_t;
using OrderId = std::uint64_t;
using ClientOrderId = std::uint64_t;
using AccountId = std::uint32_t;
using SymbolId = std::uint32_t;
using Price = std::int64_t;     // integer ticks
using Qty = std::uint32_t;
using TimestampNs = std::uint64_t;

constexpr SeqNo kInvalidSeq = 0;
constexpr OrderId kInvalidOrderId = 0;
constexpr std::uint32_t kProtocolMagic = 0x45585349u; // EXSI
constexpr std::uint16_t kProtocolVersion = 1;

using Bytes = std::vector<std::byte>;

enum class Side : std::uint8_t { Buy = 1, Sell = 2 };
enum class CommandType : std::uint8_t { New = 1, Cancel = 2, Replace = 3 };
enum class TimeInForce : std::uint8_t { Gtc = 1, Ioc = 2 };
enum class RejectReason : std::uint8_t {
  None = 0,
  Malformed = 1,
  UnknownOrder = 2,
  DuplicateOrder = 3,
  RiskMaxQty = 4,
  RiskMaxNotional = 5,
  RiskOpenOrders = 6,
  RiskRateLimit = 7,
  InvalidSymbol = 8,
  InvalidQty = 9,
  InvalidPrice = 10,
};

struct ClientCommand {
  CommandType type{CommandType::New};
  ClientOrderId client_order_id{0};
  OrderId order_id{0};        // user provided stable order id in this simulator
  OrderId cancel_order_id{0}; // cancel/replace target
  AccountId account_id{0};
  SymbolId symbol_id{0};
  Side side{Side::Buy};
  Price price{0};
  Qty qty{0};
  TimeInForce tif{TimeInForce::Gtc};
  TimestampNs client_ts_ns{0};
};

struct SequencedCommand {
  SeqNo seq{0};
  TimestampNs recv_ts_ns{0};
  ClientCommand cmd{};
};

struct Ack {
  SeqNo seq{0};
  ClientOrderId client_order_id{0};
  OrderId order_id{0};
  bool accepted{false};
  RejectReason reason{RejectReason::None};
};

struct Trade {
  SeqNo seq{0};
  SymbolId symbol_id{0};
  OrderId resting_order_id{0};
  OrderId aggressing_order_id{0};
  Price price{0};
  Qty qty{0};
};

enum class MdType : std::uint8_t { Add = 1, Cancel = 2, Execute = 3, Replace = 4, Reject = 5 };

struct MdEvent {
  SeqNo seq{0};
  MdType type{MdType::Add};
  SymbolId symbol_id{0};
  OrderId order_id{0};
  OrderId new_order_id{0}; // replace target id
  AccountId account_id{0};
  Side side{Side::Buy};
  Price price{0};
  Qty qty{0};       // for Add/Replace: resting qty. for Cancel/Execute: delta qty
  RejectReason reason{RejectReason::None};
};

inline const char* to_string(Side s) noexcept { return s == Side::Buy ? "B" : "S"; }

inline const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::Malformed: return "malformed";
    case RejectReason::UnknownOrder: return "unknown_order";
    case RejectReason::DuplicateOrder: return "duplicate_order";
    case RejectReason::RiskMaxQty: return "risk_max_qty";
    case RejectReason::RiskMaxNotional: return "risk_max_notional";
    case RejectReason::RiskOpenOrders: return "risk_open_orders";
    case RejectReason::RiskRateLimit: return "risk_rate_limit";
    case RejectReason::InvalidSymbol: return "invalid_symbol";
    case RejectReason::InvalidQty: return "invalid_qty";
    case RejectReason::InvalidPrice: return "invalid_price";
  }
  return "unknown";
}

} // namespace exsim
