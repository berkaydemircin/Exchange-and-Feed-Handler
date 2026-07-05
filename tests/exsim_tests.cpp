#include "exsim/core/exchange.hpp"
#include "exsim/core/subscriber.hpp"
#include "exsim/md/market_data.hpp"
#include "exsim/protocol/order_entry.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace exsim;

#define REQUIRE(expr) do { if (!(expr)) { std::cerr << "REQUIRE failed: " #expr << " at " << __FILE__ << ':' << __LINE__ << '\n'; std::exit(1); } } while (false)

static ClientCommand new_order(OrderId id, AccountId acct, SymbolId sym, Side side, Price px, Qty qty) {
  ClientCommand c;
  c.type = CommandType::New;
  c.client_order_id = id + 1000;
  c.order_id = id;
  c.account_id = acct;
  c.symbol_id = sym;
  c.side = side;
  c.price = px;
  c.qty = qty;
  c.tif = TimeInForce::Gtc;
  c.client_ts_ns = id;
  return c;
}

static void test_order_entry_roundtrip() {
  auto c = new_order(42, 7, 3, Side::Sell, 12345, 99);
  auto frame = exsim::protocol::encode_client_command(c);
  auto decoded = exsim::protocol::decode_client_command(frame);
  REQUIRE(decoded.command.has_value());
  REQUIRE(decoded.command->order_id == 42);
  REQUIRE(decoded.command->symbol_id == 3);
  REQUIRE(decoded.command->side == Side::Sell);
  REQUIRE(decoded.command->price == 12345);
  frame.back() = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(frame.back()) ^ 0xFFu)};
  auto bad = exsim::protocol::decode_client_command(frame);
  REQUIRE(!bad.command.has_value());
}

static void test_market_data_gap_detection() {
  MdEvent e1{1, MdType::Add, 1, 10, 0, 1, Side::Buy, 100, 10, RejectReason::None};
  MdEvent e3{3, MdType::Add, 1, 11, 0, 1, Side::Buy, 99, 10, RejectReason::None};
  exsim::md::FeedDecoder dec(1);
  REQUIRE(dec.decode(exsim::md::encode_md_event(e1)).event.has_value());
  auto gap = dec.decode(exsim::md::encode_md_event(e3));
  REQUIRE(!gap.event.has_value());
  REQUIRE(gap.error.has_value());
  REQUIRE(gap.error->expected == 2);
  REQUIRE(gap.error->actual == 3);
}

static void test_exchange_subscriber_checksum() {
  core::ExchangeConfig cfg;
  cfg.max_symbol = 4;
  cfg.risk_limits.max_msgs_per_window = 100000;
  core::Exchange ex(cfg);
  core::SubscriberBook sub(cfg.max_symbol);

  auto apply = [&](const ClientCommand& c) {
    auto frame = protocol::encode_client_command(c);
    auto r = ex.process_frame(frame, c.client_ts_ns);
    REQUIRE(r.ack.accepted);
    for (const auto& ev : r.md_events) {
      auto md_frame = md::encode_md_event(ev);
      auto err = sub.apply_frame(md_frame);
      if (err) { std::cerr << "subscriber error: " << *err << '\n'; std::exit(1); }
    }
    return r;
  };

  apply(new_order(1, 1, 1, Side::Buy, 100, 10));
  apply(new_order(2, 1, 1, Side::Buy, 100, 5));
  auto r = apply(new_order(3, 2, 1, Side::Sell, 99, 12));
  REQUIRE(!r.trades.empty());
  REQUIRE(ex.books().checksum() == sub.checksum());
  REQUIRE(ex.live_orders() == sub.live_orders());
}

static void test_risk_reject() {
  core::ExchangeConfig cfg;
  cfg.max_symbol = 2;
  cfg.risk_limits.max_order_qty = 10;
  core::Exchange ex(cfg);
  auto c = new_order(1, 1, 1, Side::Buy, 100, 11);
  auto r = ex.process_command(c, 1);
  REQUIRE(!r.ack.accepted);
  REQUIRE(r.ack.reason == RejectReason::RiskMaxQty);
}

static void test_journal_replay() {
  const std::string path = "/tmp/exsim_test_journal.bin";
  std::filesystem::remove(path);
  core::ExchangeConfig cfg;
  cfg.max_symbol = 8;
  cfg.journal_path = path;
  core::Exchange ex(cfg);
  for (OrderId i = 1; i <= 1000; ++i) {
    Side side = (i % 3 == 0) ? Side::Sell : Side::Buy;
    Price px = side == Side::Buy ? 10000 - static_cast<Price>(i % 20) : 10010 + static_cast<Price>(i % 20);
    auto c = new_order(i, static_cast<AccountId>((i % 5) + 1), static_cast<SymbolId>((i % 8) + 1), side, px, static_cast<Qty>((i % 10) + 1));
    auto r = ex.process_command(c, i * 100);
    REQUIRE(r.ack.accepted);
  }
  ex.flush_journal();
  core::ExchangeConfig replay_cfg;
  replay_cfg.max_symbol = 8;
  auto rr = core::replay_journal(path, replay_cfg);
  REQUIRE(!rr.error.has_value());
  REQUIRE(rr.records == 1000);
  REQUIRE(rr.book_checksum == ex.books().checksum());
  REQUIRE(rr.md_checksum == ex.md_checksum());
  std::filesystem::remove(path);
}

int main() {
  test_order_entry_roundtrip();
  test_market_data_gap_detection();
  test_exchange_subscriber_checksum();
  test_risk_reject();
  test_journal_replay();
  std::cout << "all exsim tests passed\n";
}
