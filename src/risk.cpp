#include "exsim/core/risk.hpp"
#include "exsim/util/checksum.hpp"

#include <algorithm>
#include <cstdlib>

namespace exsim::core {

RiskEngine::RiskEngine(RiskLimits limits) : limits_(limits) {
  accounts_.reserve(1024);
}

std::uint64_t RiskEngine::notional(Price price, Qty qty) const noexcept {
  const auto p = price < 0 ? static_cast<std::uint64_t>(-price)
                           : static_cast<std::uint64_t>(price);
  return p * static_cast<std::uint64_t>(qty);
}

RejectReason RiskEngine::check(const ClientCommand &cmd,
                               const BookManager &books, TimestampNs now_ns) {
  auto &a = accounts_[cmd.account_id];
  while (!a.recent_msgs.empty() && now_ns >= a.recent_msgs.front() &&
         now_ns - a.recent_msgs.front() > limits_.rate_window_ns) {
    a.recent_msgs.pop_front();
  }
  if (a.recent_msgs.size() >= limits_.max_msgs_per_window)
    return RejectReason::RiskRateLimit;
  a.recent_msgs.push_back(now_ns);

  if (cmd.type == CommandType::Cancel) {
    if (!books.contains_order(cmd.cancel_order_id))
      return RejectReason::UnknownOrder;
    return RejectReason::None;
  }
  if (cmd.type == CommandType::Replace) {
    if (!books.contains_order(cmd.cancel_order_id))
      return RejectReason::UnknownOrder;
  }
  if (cmd.qty > limits_.max_order_qty)
    return RejectReason::RiskMaxQty;
  if (notional(cmd.price, cmd.qty) > limits_.max_order_notional)
    return RejectReason::RiskMaxNotional;
  if (a.open_orders >= limits_.max_open_orders_per_account &&
      cmd.type == CommandType::New)
    return RejectReason::RiskOpenOrders;
  return RejectReason::None;
}

void RiskEngine::on_market_data(const MdEvent &ev, bool order_live_after) {
  auto &a = accounts_[ev.account_id];
  const auto n = notional(ev.price, ev.qty);
  if (ev.type == MdType::Add) {
    ++a.open_orders;
    a.open_notional += n;
    return;
  }
  if (ev.type == MdType::Cancel) {
    if (a.open_orders > 0)
      --a.open_orders;
    a.open_notional = a.open_notional > n ? a.open_notional - n : 0;
    return;
  }
  if (ev.type == MdType::Execute) {
    if (!order_live_after && a.open_orders > 0)
      --a.open_orders;
    a.open_notional = a.open_notional > n ? a.open_notional - n : 0;
  }
}

std::uint64_t RiskEngine::checksum() const {
  std::uint64_t h = 0x5249534b43484b53ull;
  for (const auto &[acct, s] : accounts_) {
    hash_combine(h, acct);
    hash_combine(h, s.open_orders);
    hash_combine(h, s.open_notional);
  }
  return h;
}

} // namespace exsim::core
