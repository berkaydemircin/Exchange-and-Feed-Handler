#include "exsim/core/exchange.hpp"
#include "exsim/core/subscriber.hpp"
#include "exsim/md/market_data.hpp"
#include "exsim/protocol/order_entry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  #include <intrin.h>
  #pragma intrinsic(__rdtsc)
  #pragma intrinsic(__rdtscp)
  #pragma intrinsic(_mm_lfence)
  #define EXSIM_HAS_TSC 1
#elif defined(__x86_64__) || defined(__i386__)
  #include <x86intrin.h>
  #define EXSIM_HAS_TSC 1
#else
  #define EXSIM_HAS_TSC 0
#endif

using namespace exsim;

namespace {

#if EXSIM_HAS_TSC
inline std::uint64_t tsc_begin() noexcept {
  _mm_lfence();
  return __rdtsc();
}

inline std::uint64_t tsc_end() noexcept {
  unsigned aux = 0;
  const auto t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

static double calibrate_ticks_per_ns() {
  const auto wall0 = std::chrono::steady_clock::now();
  const auto c0 = tsc_begin();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto c1 = tsc_end();
  const auto wall1 = std::chrono::steady_clock::now();
  const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count());
  return static_cast<double>(c1 - c0) / ns;
}
#else
inline std::uint64_t tsc_begin() noexcept {
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
inline std::uint64_t tsc_end() noexcept { return tsc_begin(); }
static double calibrate_ticks_per_ns() { return 1.0; }
#endif

struct Latencies {
  std::vector<std::uint64_t> cycles;
  bool sorted{false};

  void add(std::uint64_t v) {
    cycles.push_back(v);
    sorted = false;
  }

  void sort_once() {
    if (!sorted) {
      std::sort(cycles.begin(), cycles.end());
      sorted = true;
    }
  }

  std::uint64_t pct(double p) {
    if (cycles.empty()) return 0;
    sort_once();
    const double last = static_cast<double>(cycles.size() - 1);
    const auto idx = static_cast<std::size_t>(std::min(last, (p / 100.0) * last));
    return cycles[idx];
  }
};

ClientCommand make_new(OrderId id, AccountId acct, SymbolId sym, Side side, Price px, Qty qty, TimestampNs ts) {
  ClientCommand c;
  c.type = CommandType::New;
  c.client_order_id = id + 1'000'000;
  c.order_id = id;
  c.account_id = acct;
  c.symbol_id = sym;
  c.side = side;
  c.price = px;
  c.qty = qty;
  c.tif = TimeInForce::Gtc;
  c.client_ts_ns = ts;
  return c;
}

double to_ns(std::uint64_t cycles, double ticks_per_ns) {
  return ticks_per_ns > 0.0 ? static_cast<double>(cycles) / ticks_per_ns : 0.0;
}

} // namespace

int main(int argc, char** argv) {
  const std::size_t events = argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 1'000'000;
  const SymbolId symbols = argc > 2 ? static_cast<SymbolId>(std::stoul(argv[2])) : 1'000;
  std::string journal_path = argc > 3 ? argv[3] : "/tmp/exsim_bench_journal.bin";

  const int half_spread = argc > 4 ? std::stoi(argv[4]) : 5;
  const int jitter_width = argc > 5 ? std::stoi(argv[5]) : 50;
  const int cancel_pct = argc > 6 ? std::stoi(argv[6]) : 30; 
  std::filesystem::remove(journal_path);

  // no mixed or dynamic rebasing currently
  core::PriceLadderConfig ladder;
  ladder.min_price = 99'900;
  ladder.max_price = 100'100;
  ladder.symbol_price_stride = 10;
  ladder.symbol_price_modulo = 100;

  core::ExchangeConfig cfg;
  cfg.max_symbol = symbols;
  cfg.ladder = ladder;
  cfg.journal_path = journal_path;
  cfg.risk_limits.max_order_qty = 1'000'000;
  cfg.risk_limits.max_order_notional = 100'000'000'000ull;
  cfg.risk_limits.max_open_orders_per_account = 5'000'000;
  cfg.risk_limits.max_msgs_per_window = 5'000'000;

  const double ticks_per_ns = calibrate_ticks_per_ns();

  core::Exchange exchange(cfg);
  core::SubscriberBook subscriber(symbols, cfg.ladder);
  std::mt19937_64 rng(0xBADC0FFEEULL);
  std::uniform_int_distribution<int> side_dist(0, 1);
  std::uniform_int_distribution<int> qty_dist(1, 100);
  std::uniform_int_distribution<int> acct_dist(1, 1024);
  std::uniform_int_distribution<int> price_jitter(-jitter_width, jitter_width);
  std::vector<OrderId> live_ids;
  live_ids.reserve(events / 2 + 1024);
  std::unordered_map<OrderId, std::size_t> live_pos;
  live_pos.reserve(events / 2 + 1024);

  auto track_add = [&](OrderId id) {
    if (live_pos.find(id) != live_pos.end()) return;
    live_pos[id] = live_ids.size();
    live_ids.push_back(id);
  };
  auto track_remove = [&](OrderId id) {
    auto it = live_pos.find(id);
    if (it == live_pos.end()) return;
    const std::size_t pos = it->second;
    const OrderId last = live_ids.back();
    live_ids[pos] = last;
    live_pos[last] = pos;
    live_ids.pop_back();
    live_pos.erase(it);
  };

  Latencies lat;
  lat.cycles.reserve(std::min<std::size_t>(events, 2'000'000));
  std::uint64_t md_frames = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;

  const auto bench_start = std::chrono::steady_clock::now();
  OrderId next_id = 1;
  for (std::size_t i = 0; i < events; ++i) {
    ClientCommand cmd;
    const bool do_cancel = !live_ids.empty() && (rng() % 100) < static_cast<unsigned>(cancel_pct);
    if (do_cancel) {
      const std::size_t idx = static_cast<std::size_t>(rng() % live_ids.size());
      const OrderId target = live_ids[idx];
      cmd.type = CommandType::Cancel;
      cmd.client_order_id = next_id + 1'000'000;
      cmd.order_id = 0;
      cmd.cancel_order_id = target;
      cmd.account_id = static_cast<AccountId>(acct_dist(rng));
      cmd.symbol_id = 1;
      cmd.side = Side::Buy;
      cmd.price = ladder.min_price;
      cmd.qty = 0;
      cmd.tif = TimeInForce::Gtc;
      cmd.client_ts_ns = static_cast<TimestampNs>(i + 1);
    } else {
      const SymbolId sym = static_cast<SymbolId>((rng() % symbols) + 1);
      const Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
      const Price mid = 100000 + static_cast<Price>((sym % 100) * 10);
      const Price px = side == Side::Buy ? mid - half_spread + price_jitter(rng) : mid + half_spread + price_jitter(rng);
      cmd = make_new(next_id, static_cast<AccountId>(acct_dist(rng)), sym, side, px, static_cast<Qty>(qty_dist(rng)), static_cast<TimestampNs>(i + 1));
      ++next_id;
    }

    const auto t0 = tsc_begin();
    const auto frame = protocol::encode_client_command_frame(cmd);
    auto result = exchange.process_frame(frame.span(), cmd.client_ts_ns);
    for (const auto& ev : result.md_events) {
      auto md_frame = md::encode_md_event_frame(ev);
      auto err = subscriber.apply_frame(md_frame.span());
      if (err) {
        std::cerr << "subscriber error at event " << i << ": " << *err << "\n";
        return 2;
      }
      ++md_frames;
    }
    const auto t1 = tsc_end();
    if (i < 2'000'000) lat.add(t1 - t0);
    for (const auto& ev : result.md_events) {
      if (ev.type == MdType::Add) track_add(ev.order_id);
      else if (ev.type == MdType::Cancel) track_remove(ev.order_id);
      else if (ev.type == MdType::Execute && !exchange.books().contains_order(ev.order_id)) track_remove(ev.order_id);
    }
    if (result.ack.accepted) ++accepted; else ++rejected;
  }
  exchange.flush_journal();
  const auto bench_end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(bench_end - bench_start).count();

  core::ExchangeConfig replay_cfg;
  replay_cfg.max_symbol = symbols;
  replay_cfg.ladder = cfg.ladder;
  replay_cfg.risk_limits = cfg.risk_limits;
  auto replay = core::replay_journal(journal_path, replay_cfg);

  const auto p50 = lat.pct(50);
  const auto p90 = lat.pct(90);
  const auto p99 = lat.pct(99);
  const auto p999 = lat.pct(99.9);

  std::cout << "=== Full-path exchange replay benchmark ===\n";
  std::cout << "events attempted       : " << events << "\n";
  std::cout << "accepted / rejected    : " << accepted << " / " << rejected << "\n";
  std::cout << "symbols                : " << symbols << "\n";
  std::cout << "price band             : [" << ladder.min_price << ", " << ladder.max_price << "]\n";
  std::cout << "md frames              : " << md_frames << "\n";
  std::cout << "live orders            : " << exchange.live_orders() << "\n";
  std::cout << "wall time              : " << seconds << " s\n";
  std::cout << "throughput             : " << static_cast<double>(events) / seconds / 1e6 << " Mevents/s\n";
  std::cout << "latency sampled events : " << lat.cycles.size() << "\n";
#if EXSIM_HAS_TSC
  std::cout << std::fixed << std::setprecision(4);
  std::cout << "tsc frequency estimate : " << ticks_per_ns << " ticks/ns (" << ticks_per_ns << " GHz effective)\n";
  std::cout.unsetf(std::ios::floatfield);
  std::cout << std::setprecision(6);
  std::cout << "latency cycles p50/p90/p99/p99.9 : " << p50 << " / " << p90 << " / " << p99 << " / " << p999 << "\n";
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "latency ns est. p50/p90/p99/p99.9 : " << to_ns(p50, ticks_per_ns) << " / " << to_ns(p90, ticks_per_ns) << " / " << to_ns(p99, ticks_per_ns) << " / " << to_ns(p999, ticks_per_ns) << "\n";
  std::cout.unsetf(std::ios::floatfield);
  std::cout << std::setprecision(6);
#else
  std::cout << "latency ticks p50/p90/p99/p99.9 : " << p50 << " / " << p90 << " / " << p99 << " / " << p999 << "\n";
#endif
  std::cout << "engine book checksum   : " << exchange.books().checksum() << "\n";
  std::cout << "subscriber checksum    : " << subscriber.checksum() << "\n";
  std::cout << "md checksum            : " << exchange.md_checksum() << "\n";
  if (replay.error) {
    std::cout << "replay error           : " << *replay.error << "\n";
    return 3;
  }
  std::cout << "replay records         : " << replay.records << "\n";
  std::cout << "replay book checksum   : " << replay.book_checksum << "\n";
  std::cout << "replay md checksum     : " << replay.md_checksum << "\n";
  const bool ok = exchange.books().checksum() == subscriber.checksum() && replay.book_checksum == exchange.books().checksum() && replay.md_checksum == exchange.md_checksum();
  std::cout << "verification           : " << (ok ? "PASS" : "FAIL") << "\n";
  return ok ? 0 : 1;
}
