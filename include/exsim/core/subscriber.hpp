#pragma once

#include "exsim/core/order_book.hpp"
#include "exsim/md/market_data.hpp"
#include <optional>
#include <string>

namespace exsim::core {

class SubscriberBook {
 public:
  explicit SubscriberBook(SymbolId max_symbol, PriceLadderConfig ladder = {});
  std::optional<std::string> apply_frame(std::span<const std::byte> md_frame);
  std::optional<std::string> apply_event(const MdEvent& ev);
  [[nodiscard]] std::uint64_t checksum() const { return books_.checksum(); }
  [[nodiscard]] std::size_t live_orders() const { return books_.live_orders(); }
  [[nodiscard]] SeqNo expected_seq() const noexcept { return decoder_.expected(); }

 private:
  BookManager books_;
  md::FeedDecoder decoder_;
};

} // namespace exsim::core
