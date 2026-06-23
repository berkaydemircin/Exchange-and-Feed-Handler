#include "exsim/core/subscriber.hpp"

namespace exsim::core {

SubscriberBook::SubscriberBook(SymbolId max_symbol, PriceLadderConfig ladder) : books_(max_symbol, ladder), decoder_(1) {}

std::optional<std::string> SubscriberBook::apply_frame(std::span<const std::byte> md_frame) {
  auto res = decoder_.decode(md_frame);
  if (!res.event) {
    if (res.error) return res.error->message;
    return std::string{"unknown decode error"};
  }
  return apply_event(*res.event);
}

std::optional<std::string> SubscriberBook::apply_event(const MdEvent& ev) {
  ClientCommand cmd;
  cmd.client_order_id = ev.seq;
  cmd.order_id = ev.order_id;
  cmd.cancel_order_id = ev.order_id;
  cmd.account_id = ev.account_id;
  cmd.symbol_id = ev.symbol_id;
  cmd.side = ev.side;
  cmd.price = ev.price;
  cmd.qty = ev.qty;
  cmd.tif = TimeInForce::Gtc;

  switch (ev.type) {
    case MdType::Add:
      if (!books_.feed_add(ev)) return std::string{"md add failed"};
      return std::nullopt;
    case MdType::Cancel:
      if (!books_.cancel_order(ev.seq, ev.order_id)) return std::string{"md cancel unknown order"};
      return std::nullopt;
    case MdType::Execute:
      if (!books_.feed_execute(ev)) return std::string{"md execute failed"};
      return std::nullopt;
    case MdType::Replace:
      // reserved for later, currently only add/cancel
      return std::nullopt;
    case MdType::Reject:
      return std::nullopt;
  }
  return std::string{"unknown md type"};
}

} // namespace exsim::core
