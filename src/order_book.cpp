#include "exsim/core/order_book.hpp"
#include "exsim/util/checksum.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace exsim::core {
namespace {
constexpr std::uint32_t bits_per_word = 64;
}

OrderBook::OrderBook(SymbolId symbol_id, PriceLadderConfig ladder)
    : symbol_id_(symbol_id), min_price_(ladder.min_price), max_price_(ladder.max_price) {
  if (min_price_ <= 0 || max_price_ < min_price_) {
    throw std::invalid_argument("invalid price ladder range");
  }
  const auto span = static_cast<std::uint64_t>(max_price_ - min_price_) + 1ull;
  if (span > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::invalid_argument("price ladder range too wide");
  }
  level_count_ = static_cast<std::uint32_t>(span);
  bids_.resize(level_count_);
  asks_.resize(level_count_);
  const std::size_t words = (static_cast<std::size_t>(level_count_) + bits_per_word - 1) / bits_per_word;
  bid_bitmap_.assign(words, 0);
  ask_bitmap_.assign(words, 0);
  nodes_.reserve(4096);
  free_.reserve(1024);
  index_.reserve(4096);
}

bool OrderBook::price_in_band(Price price) const noexcept {
  return price >= min_price_ && price <= max_price_;
}

std::uint32_t OrderBook::price_to_index(Price price) const noexcept {
  return static_cast<std::uint32_t>(price - min_price_);
}

Price OrderBook::index_to_price(std::uint32_t idx) const noexcept {
  return min_price_ + static_cast<Price>(idx);
}

bool OrderBook::has_bit(const std::vector<std::uint64_t>& bitmap, std::uint32_t idx) const noexcept {
  const std::size_t w = idx / bits_per_word;
  const std::uint32_t b = idx % bits_per_word;
  return (bitmap[w] & (1ull << b)) != 0;
}

void OrderBook::set_occupied(Side side, std::uint32_t idx) {
  auto& bm = side == Side::Buy ? bid_bitmap_ : ask_bitmap_;
  const std::size_t w = idx / bits_per_word;
  const std::uint32_t b = idx % bits_per_word;
  bm[w] |= (1ull << b);
  if (side == Side::Buy) {
    if (!best_bid_idx_ || idx > *best_bid_idx_) best_bid_idx_ = idx;
  } else {
    if (!best_ask_idx_ || idx < *best_ask_idx_) best_ask_idx_ = idx;
  }
}

void OrderBook::clear_occupied(Side side, std::uint32_t idx) {
  auto& bm = side == Side::Buy ? bid_bitmap_ : ask_bitmap_;
  const std::size_t w = idx / bits_per_word;
  const std::uint32_t b = idx % bits_per_word;
  bm[w] &= ~(1ull << b);
  recompute_best_after_clear(side, idx);
}

std::optional<std::uint32_t> OrderBook::find_lowest_occupied(const std::vector<std::uint64_t>& bitmap) const noexcept {
  for (std::size_t w = 0; w < bitmap.size(); ++w) {
    const std::uint64_t word = bitmap[w];
    if (word != 0) {
      const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
      const auto idx = static_cast<std::uint32_t>(w * bits_per_word + bit);
      if (idx < level_count_) return idx;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> OrderBook::find_highest_occupied(const std::vector<std::uint64_t>& bitmap) const noexcept {
  for (std::size_t w = bitmap.size(); w > 0; --w) {
    const std::uint64_t word = bitmap[w - 1];
    if (word != 0) {
      const auto bit = static_cast<std::uint32_t>(63u - static_cast<std::uint32_t>(std::countl_zero(word)));
      const auto idx = static_cast<std::uint32_t>((w - 1) * bits_per_word + bit);
      if (idx < level_count_) return idx;
    }
  }
  return std::nullopt;
}

void OrderBook::recompute_best_after_clear(Side side, std::uint32_t cleared_idx) {
  if (side == Side::Buy) {
    if (best_bid_idx_ && *best_bid_idx_ == cleared_idx) best_bid_idx_ = find_highest_occupied(bid_bitmap_);
  } else {
    if (best_ask_idx_ && *best_ask_idx_ == cleared_idx) best_ask_idx_ = find_lowest_occupied(ask_bitmap_);
  }
}

OrderBook::Level& OrderBook::level_for(Side side, std::uint32_t idx) noexcept {
  return side == Side::Buy ? bids_[idx] : asks_[idx];
}

const OrderBook::Level& OrderBook::level_for(Side side, std::uint32_t idx) const noexcept {
  return side == Side::Buy ? bids_[idx] : asks_[idx];
}

bool OrderBook::contains(OrderId id) const { return index_.find(id) != index_.end(); }

std::optional<RestingOrderView> OrderBook::find(OrderId id) const {
  auto it = index_.find(id);
  if (it == index_.end()) return std::nullopt;
  const Node& n = nodes_[it->second];
  if (!n.active) return std::nullopt;
  return RestingOrderView{n.order_id, n.account_id, n.side, n.price, n.qty};
}

std::uint32_t OrderBook::alloc_node(OrderId id, AccountId account_id, Side side, Price price, Qty qty) {
  std::uint32_t idx;
  if (!free_.empty()) {
    idx = free_.back();
    free_.pop_back();
    nodes_[idx] = Node{};
  } else {
    idx = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back(Node{});
  }
  Node& n = nodes_[idx];
  n.order_id = id;
  n.account_id = account_id;
  n.side = side;
  n.price = price;
  n.qty = qty;
  n.level_idx = price_to_index(price);
  n.active = true;
  index_[id] = idx;
  return idx;
}

void OrderBook::free_node(std::uint32_t idx) {
  Node& n = nodes_[idx];
  index_.erase(n.order_id);
  n = Node{};
  free_.push_back(idx);
}

void OrderBook::append_to_level(std::uint32_t idx) {
  Node& n = nodes_[idx];
  Level& level = level_for(n.side, n.level_idx);
  const bool was_empty = level.count == 0;

  n.prev = level.tail;
  n.next = kNull;
  if (level.tail != kNull) nodes_[level.tail].next = idx;
  else level.head = idx;
  level.tail = idx;
  level.total_qty += n.qty;
  ++level.count;

  if (was_empty) {
    if (n.side == Side::Buy) ++bid_level_count_;
    else ++ask_level_count_;
    set_occupied(n.side, n.level_idx);
  }
}

MdEvent OrderBook::remove_node(SeqNo seq, std::uint32_t idx, Qty removed_qty, MdType type) {
  Node& n = nodes_[idx];
  const auto side = n.side;
  const auto level_idx = n.level_idx;
  Level& level = level_for(side, level_idx);

  if (n.prev != kNull) nodes_[n.prev].next = n.next;
  else level.head = n.next;
  if (n.next != kNull) nodes_[n.next].prev = n.prev;
  else level.tail = n.prev;

  level.total_qty -= std::min(level.total_qty, removed_qty);
  if (level.count > 0) --level.count;

  MdEvent ev{seq, type, symbol_id_, n.order_id, 0, n.account_id, n.side, n.price, removed_qty, RejectReason::None};

  if (level.count == 0) {
    level.head = kNull;
    level.tail = kNull;
    level.total_qty = 0;
    if (side == Side::Buy) --bid_level_count_;
    else --ask_level_count_;
    clear_occupied(side, level_idx);
  }
  free_node(idx);
  return ev;
}

bool OrderBook::crosses(Side side, Price price) const {
  if (side == Side::Buy) return best_ask_idx_ && price >= index_to_price(*best_ask_idx_);
  return best_bid_idx_ && price <= index_to_price(*best_bid_idx_);
}

Qty OrderBook::execute_head(SeqNo seq, Level& level, std::uint32_t idx, Qty qty, std::vector<MdEvent>& md, std::vector<Trade>& trades, OrderId aggressor) {
  Node& resting = nodes_[idx];
  const Qty exec_qty = std::min(resting.qty, qty);
  resting.qty -= exec_qty;
  level.total_qty -= exec_qty;
  trades.push_back(Trade{seq, symbol_id_, resting.order_id, aggressor, resting.price, exec_qty});
  md.push_back(MdEvent{seq, MdType::Execute, symbol_id_, resting.order_id, 0, resting.account_id, resting.side, resting.price, exec_qty, RejectReason::None});
  if (resting.qty == 0) {
    (void)remove_node(seq, idx, 0, MdType::Execute); // already emitted execute event, remove without emitting another md event
  }
  return exec_qty;
}

MatchOutput OrderBook::add_order(SeqNo seq, OrderId order_id, AccountId account_id, Side side, Price price, Qty qty, TimeInForce tif) {
  MatchOutput out;
  out.remaining_qty = qty;
  if (qty == 0) {
    out.reject = RejectReason::InvalidQty;
    return out;
  }
  if (!price_in_band(price)) {
    out.reject = RejectReason::InvalidPrice;
    return out;
  }
  if (contains(order_id)) {
    out.reject = RejectReason::DuplicateOrder;
    return out;
  }

  while (out.remaining_qty > 0 && crosses(side, price)) {
    if (side == Side::Buy) {
      const std::uint32_t idx_level = *best_ask_idx_;
      Level& level = asks_[idx_level];
      const std::uint32_t idx = level.head;
      const Qty done = execute_head(seq, level, idx, out.remaining_qty, out.md_events, out.trades, order_id);
      out.remaining_qty -= done;
    } else {
      const std::uint32_t idx_level = *best_bid_idx_;
      Level& level = bids_[idx_level];
      const std::uint32_t idx = level.head;
      const Qty done = execute_head(seq, level, idx, out.remaining_qty, out.md_events, out.trades, order_id);
      out.remaining_qty -= done;
    }
  }

  if (out.remaining_qty > 0 && tif == TimeInForce::Gtc) {
    const auto idx = alloc_node(order_id, account_id, side, price, out.remaining_qty);
    append_to_level(idx);
    out.md_events.push_back(MdEvent{seq, MdType::Add, symbol_id_, order_id, 0, account_id, side, price, out.remaining_qty, RejectReason::None});
    out.remaining_qty = 0;
  }
  return out;
}

std::optional<MdEvent> OrderBook::cancel_order(SeqNo seq, OrderId order_id) {
  auto it = index_.find(order_id);
  if (it == index_.end()) return std::nullopt;
  const Qty qty = nodes_[it->second].qty;
  return remove_node(seq, it->second, qty, MdType::Cancel);
}

bool OrderBook::feed_add(OrderId order_id, AccountId account_id, Side side, Price price, Qty qty) {
  if (contains(order_id) || qty == 0 || !price_in_band(price)) return false;
  const auto idx = alloc_node(order_id, account_id, side, price, qty);
  append_to_level(idx);
  return true;
}

bool OrderBook::feed_execute(OrderId order_id, Qty qty) {
  auto it = index_.find(order_id);
  if (it == index_.end() || qty == 0) return false;
  Node& n = nodes_[it->second];
  Level& level = level_for(n.side, n.level_idx);
  if (qty >= n.qty) {
    (void)remove_node(0, it->second, n.qty, MdType::Execute);
    return true;
  }
  n.qty -= qty;
  level.total_qty -= qty;
  return true;
}

MatchOutput OrderBook::replace_order(SeqNo seq, OrderId old_id, OrderId new_id, AccountId account_id, Side side, Price price, Qty qty, TimeInForce tif) {
  MatchOutput out;
  if (!price_in_band(price)) {
    out.reject = RejectReason::InvalidPrice;
    return out;
  }
  auto old = cancel_order(seq, old_id);
  if (!old) {
    out.reject = RejectReason::UnknownOrder;
    return out;
  }
  out.md_events.push_back(*old);
  auto add = add_order(seq, new_id, account_id, side, price, qty, tif);
  out.trades.insert(out.trades.end(), add.trades.begin(), add.trades.end());
  out.md_events.insert(out.md_events.end(), add.md_events.begin(), add.md_events.end());
  out.remaining_qty = add.remaining_qty;
  out.reject = add.reject;
  return out;
}

BookStats OrderBook::stats() const {
  BookStats s;
  s.live_orders = index_.size();
  s.bid_levels = bid_level_count_;
  s.ask_levels = ask_level_count_;
  if (best_bid_idx_) s.best_bid = index_to_price(*best_bid_idx_);
  if (best_ask_idx_) s.best_ask = index_to_price(*best_ask_idx_);
  return s;
}

std::uint64_t OrderBook::checksum() const {
  std::uint64_t h = 0x45584348424f4f4bull;
  hash_combine(h, symbol_id_);
  for (auto maybe_idx = find_highest_occupied(bid_bitmap_); maybe_idx;) {
    const std::uint32_t idx = *maybe_idx;
    const Level& level = bids_[idx];
    hash_combine(h, static_cast<std::uint64_t>(index_to_price(idx)));
    hash_combine(h, level.total_qty);
    auto node_idx = level.head;
    while (node_idx != kNull) {
      const Node& n = nodes_[node_idx];
      hash_combine(h, n.order_id);
      hash_combine(h, n.account_id);
      hash_combine(h, n.qty);
      node_idx = n.next;
    }
    if (idx == 0) break;
    std::uint64_t prev_found = 0;
    bool found = false;
    for (std::uint32_t j = idx; j-- > 0;) {
      if (has_bit(bid_bitmap_, j)) { prev_found = j; found = true; break; }
    }
    maybe_idx = found ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(prev_found)} : std::nullopt;
  }

  for (auto maybe_idx = find_lowest_occupied(ask_bitmap_); maybe_idx;) {
    const std::uint32_t idx = *maybe_idx;
    const Level& level = asks_[idx];
    hash_combine(h, static_cast<std::uint64_t>(index_to_price(idx)));
    hash_combine(h, level.total_qty);
    auto node_idx = level.head;
    while (node_idx != kNull) {
      const Node& n = nodes_[node_idx];
      hash_combine(h, n.order_id);
      hash_combine(h, n.account_id);
      hash_combine(h, n.qty);
      node_idx = n.next;
    }
    std::optional<std::uint32_t> next;
    for (std::uint32_t j = idx + 1; j < level_count_; ++j) {
      if (has_bit(ask_bitmap_, j)) { next = j; break; }
    }
    maybe_idx = next;
  }
  return h;
}

BookManager::BookManager(SymbolId max_symbol, PriceLadderConfig ladder)
    : max_symbol_(max_symbol), ladder_(ladder) {
  books_.reserve(static_cast<std::size_t>(max_symbol_) + 1);
  auto symbol_ladder = [this](SymbolId s) {
    PriceLadderConfig cfg = ladder_;
    if (cfg.symbol_price_stride != 0 && cfg.symbol_price_modulo != 0) {
      const Price offset = static_cast<Price>(s % cfg.symbol_price_modulo) * cfg.symbol_price_stride;
      cfg.min_price += offset;
      cfg.max_price += offset;
      cfg.symbol_price_stride = 0;
      cfg.symbol_price_modulo = 0;
    }
    return cfg;
  };
  books_.emplace_back(0, symbol_ladder(0)); // slot zero intentionally unused
  for (SymbolId s = 1; s <= max_symbol_; ++s) books_.emplace_back(s, symbol_ladder(s));
  order_to_symbol_.reserve(65536);
}

OrderBook& BookManager::book(SymbolId symbol_id) {
  if (symbol_id == 0 || symbol_id > max_symbol_) throw std::out_of_range("bad symbol id");
  return books_[symbol_id];
}

const OrderBook* BookManager::find_book(SymbolId symbol_id) const {
  if (symbol_id == 0 || symbol_id > max_symbol_) return nullptr;
  return &books_[symbol_id];
}

bool BookManager::contains_order(OrderId order_id) const { return order_to_symbol_.find(order_id) != order_to_symbol_.end(); }

SymbolId BookManager::order_symbol(OrderId order_id) const {
  auto it = order_to_symbol_.find(order_id);
  if (it == order_to_symbol_.end()) return 0;
  return it->second;
}

std::optional<RestingOrderView> BookManager::find_order(OrderId order_id) const {
  const SymbolId s = order_symbol(order_id);
  if (s == 0) return std::nullopt;
  return books_[s].find(order_id);
}

void BookManager::refresh_index_after_add(SymbolId sym, const MatchOutput&, OrderId maybe_added_id) {
  if (books_[sym].contains(maybe_added_id)) order_to_symbol_[maybe_added_id] = sym;
}

void BookManager::remove_index(OrderId id) { order_to_symbol_.erase(id); }

MatchOutput BookManager::add_order(SeqNo seq, const ClientCommand& cmd) {
  auto& b = book(cmd.symbol_id);
  auto out = b.add_order(seq, cmd.order_id, cmd.account_id, cmd.side, cmd.price, cmd.qty, cmd.tif);
  for (const auto& ev : out.md_events) {
    if (ev.type == MdType::Execute || ev.type == MdType::Cancel) {
      if (!b.contains(ev.order_id)) remove_index(ev.order_id);
    }
  }
  refresh_index_after_add(cmd.symbol_id, out, cmd.order_id);
  return out;
}

std::optional<MdEvent> BookManager::cancel_order(SeqNo seq, OrderId order_id) {
  const SymbolId s = order_symbol(order_id);
  if (s == 0) return std::nullopt;
  auto ev = books_[s].cancel_order(seq, order_id);
  if (ev) remove_index(order_id);
  return ev;
}

bool BookManager::feed_add(const MdEvent& ev) {
  if (ev.symbol_id == 0 || ev.symbol_id > max_symbol_) return false;
  if (!books_[ev.symbol_id].feed_add(ev.order_id, ev.account_id, ev.side, ev.price, ev.qty)) return false;
  order_to_symbol_[ev.order_id] = ev.symbol_id;
  return true;
}

bool BookManager::feed_execute(const MdEvent& ev) {
  const SymbolId s = order_symbol(ev.order_id);
  if (s == 0) return false;
  const bool ok = books_[s].feed_execute(ev.order_id, ev.qty);
  if (ok && !books_[s].contains(ev.order_id)) remove_index(ev.order_id);
  return ok;
}

MatchOutput BookManager::replace_order(SeqNo seq, const ClientCommand& cmd) {
  const SymbolId s = order_symbol(cmd.cancel_order_id);
  if (s == 0) return MatchOutput{{}, {}, 0, RejectReason::UnknownOrder};
  auto out = books_[s].replace_order(seq, cmd.cancel_order_id, cmd.order_id, cmd.account_id, cmd.side, cmd.price, cmd.qty, cmd.tif);
  remove_index(cmd.cancel_order_id);
  for (const auto& ev : out.md_events) {
    if ((ev.type == MdType::Execute || ev.type == MdType::Cancel) && !books_[s].contains(ev.order_id)) remove_index(ev.order_id);
  }
  refresh_index_after_add(s, out, cmd.order_id);
  return out;
}

std::uint64_t BookManager::checksum() const {
  std::uint64_t h = 0x424f4f4b4d475231ull;
  for (SymbolId s = 1; s <= max_symbol_; ++s) hash_combine(h, books_[s].checksum());
  return h;
}

std::size_t BookManager::live_orders() const {
  std::size_t n = 0;
  for (SymbolId s = 1; s <= max_symbol_; ++s) n += books_[s].stats().live_orders;
  return n;
}

} // namespace exsim::core
