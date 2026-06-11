#pragma once

#include "exsim/core/order_book.hpp"
#include "exsim/types.hpp"
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace exsim::core {

struct RiskLimits {
  Qty max_order_qty{100000};
  std::uint64_t max_order_notional{10'000'000'000ull};
  std::uint32_t max_open_orders_per_account{100000};
  std::uint32_t max_msgs_per_window{100000};
  TimestampNs rate_window_ns{1'000'000'000ull};
};

class RiskEngine {
 public:
  explicit RiskEngine(RiskLimits limits = {});

  RejectReason check(const ClientCommand& cmd, const BookManager& books, TimestampNs now_ns);
  void on_market_data(const MdEvent& ev, bool order_live_after);
  [[nodiscard]] std::uint64_t checksum() const;

 private:
  struct AccountState {
    std::uint32_t open_orders{0};
    std::uint64_t open_notional{0};
    std::deque<TimestampNs> recent_msgs;
  };

  RiskLimits limits_;
  std::unordered_map<AccountId, AccountState> accounts_;

  [[nodiscard]] std::uint64_t notional(Price price, Qty qty) const noexcept;
};

} // namespace exsim::core
