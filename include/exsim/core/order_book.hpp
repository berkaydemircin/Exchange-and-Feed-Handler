#pragma once

#include "exsim/types.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace exsim::core {

struct RestingOrderView {
  OrderId order_id{0};
  AccountId account_id{0};
  Side side{Side::Buy};
  Price price{0};
  Qty qty{0};
};

struct MatchOutput {
  std::vector<MdEvent> md_events;
  std::vector<Trade> trades;
  Qty remaining_qty{0};
  RejectReason reject{RejectReason::None};
};

struct BookStats {
  std::size_t live_orders{0};
  std::size_t bid_levels{0};
  std::size_t ask_levels{0};
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
};

struct PriceLadderConfig {
  // use narrower range in bench
  Price min_price{1};
  Price max_price{200'000};
  
  Price symbol_price_stride{0};
  SymbolId symbol_price_modulo{0};
};

class OrderBook {
 public:
  explicit OrderBook(SymbolId symbol_id = 0, PriceLadderConfig ladder = {});

  [[nodiscard]] SymbolId symbol_id() const noexcept { return symbol_id_; }
  [[nodiscard]] bool contains(OrderId id) const;
  [[nodiscard]] std::optional<RestingOrderView> find(OrderId id) const;

  MatchOutput add_order(SeqNo seq, OrderId order_id, AccountId account_id, Side side, Price price, Qty qty, TimeInForce tif);
  std::optional<MdEvent> cancel_order(SeqNo seq, OrderId order_id);
  bool feed_add(OrderId order_id, AccountId account_id, Side side, Price price, Qty qty);
  bool feed_execute(OrderId order_id, Qty qty);
  MatchOutput replace_order(SeqNo seq, OrderId old_id, OrderId new_id, AccountId account_id, Side side, Price price, Qty qty, TimeInForce tif);

  [[nodiscard]] BookStats stats() const;
  [[nodiscard]] std::uint64_t checksum() const;
  [[nodiscard]] bool price_in_band(Price price) const noexcept;
  [[nodiscard]] Price min_price() const noexcept { return min_price_; }
  [[nodiscard]] Price max_price() const noexcept { return max_price_; }

 private:
  static constexpr std::uint32_t kNull = UINT32_MAX;

  struct Node {
    OrderId order_id{0};
    AccountId account_id{0};
    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};
    std::uint32_t level_idx{0};
    std::uint32_t prev{kNull};
    std::uint32_t next{kNull};
    bool active{false};
  };

  struct Level {
    std::uint32_t head{kNull};
    std::uint32_t tail{kNull};
    Qty total_qty{0};
    std::uint32_t count{0};
  };

  SymbolId symbol_id_{0};
  Price min_price_{1};
  Price max_price_{200'000};
  std::uint32_t level_count_{0};

  std::vector<Node> nodes_;
  std::vector<std::uint32_t> free_;
  std::unordered_map<OrderId, std::uint32_t> index_;

  std::vector<Level> bids_;
  std::vector<Level> asks_;
  std::vector<std::uint64_t> bid_bitmap_;
  std::vector<std::uint64_t> ask_bitmap_;
  std::size_t bid_level_count_{0};
  std::size_t ask_level_count_{0};
  std::optional<std::uint32_t> best_bid_idx_;
  std::optional<std::uint32_t> best_ask_idx_;

  [[nodiscard]] std::uint32_t price_to_index(Price price) const noexcept;
  [[nodiscard]] Price index_to_price(std::uint32_t idx) const noexcept;
  [[nodiscard]] bool has_bit(const std::vector<std::uint64_t>& bitmap, std::uint32_t idx) const noexcept;
  void set_occupied(Side side, std::uint32_t idx);
  void clear_occupied(Side side, std::uint32_t idx);
  [[nodiscard]] std::optional<std::uint32_t> find_lowest_occupied(const std::vector<std::uint64_t>& bitmap) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> find_highest_occupied(const std::vector<std::uint64_t>& bitmap) const noexcept;
  void recompute_best_after_clear(Side side, std::uint32_t cleared_idx);

  [[nodiscard]] Level& level_for(Side side, std::uint32_t idx) noexcept;
  [[nodiscard]] const Level& level_for(Side side, std::uint32_t idx) const noexcept;

  std::uint32_t alloc_node(OrderId id, AccountId account_id, Side side, Price price, Qty qty);
  void free_node(std::uint32_t idx);
  void append_to_level(std::uint32_t idx);
  MdEvent remove_node(SeqNo seq, std::uint32_t idx, Qty removed_qty, MdType type);
  Qty execute_head(SeqNo seq, Level& level, std::uint32_t idx, Qty qty, std::vector<MdEvent>& md, std::vector<Trade>& trades, OrderId aggressor);
  [[nodiscard]] bool crosses(Side side, Price price) const;
};

class BookManager {
 public:
  explicit BookManager(SymbolId max_symbol = 1, PriceLadderConfig ladder = {});
  OrderBook& book(SymbolId symbol_id);
  const OrderBook* find_book(SymbolId symbol_id) const;
  bool contains_order(OrderId order_id) const;
  std::optional<RestingOrderView> find_order(OrderId order_id) const;
  SymbolId order_symbol(OrderId order_id) const;

  MatchOutput add_order(SeqNo seq, const ClientCommand& cmd);
  std::optional<MdEvent> cancel_order(SeqNo seq, OrderId order_id);
  bool feed_add(const MdEvent& ev);
  bool feed_execute(const MdEvent& ev);
  MatchOutput replace_order(SeqNo seq, const ClientCommand& cmd);

  [[nodiscard]] std::uint64_t checksum() const;
  [[nodiscard]] std::size_t live_orders() const;
  [[nodiscard]] SymbolId max_symbol() const noexcept { return max_symbol_; }
  [[nodiscard]] PriceLadderConfig ladder_config() const noexcept { return ladder_; }

 private:
  SymbolId max_symbol_{1};
  PriceLadderConfig ladder_{};
  std::vector<OrderBook> books_; // index by symbol id, slot zero unused
  std::unordered_map<OrderId, SymbolId> order_to_symbol_;
  void refresh_index_after_add(SymbolId sym, const MatchOutput& out, OrderId maybe_added_id);
  void remove_index(OrderId id);
};

} // namespace exsim::core
