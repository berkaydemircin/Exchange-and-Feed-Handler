#include "exsim/core/exchange.hpp"
#include "exsim/protocol/order_entry.hpp"
#include "exsim/util/checksum.hpp"

#include <sstream>

namespace exsim::core {

Exchange::Exchange(ExchangeConfig cfg)
    : ladder_(cfg.ladder), books_(cfg.max_symbol, cfg.ladder), risk_(cfg.risk_limits) {
  if (cfg.journal_path) journal_ = std::make_unique<journal::JournalWriter>(*cfg.journal_path);
}

ProcessResult Exchange::reject(SeqNo seq, const ClientCommand& cmd, RejectReason reason) {
  MdEvent ev{next_md_seq_++, MdType::Reject, cmd.symbol_id, cmd.order_id, 0, cmd.account_id, cmd.side, cmd.price, cmd.qty, reason};
  hash_combine(md_checksum_, ev.seq);
  hash_combine(md_checksum_, static_cast<std::uint8_t>(ev.type));
  hash_combine(md_checksum_, ev.order_id);
  hash_combine(md_checksum_, static_cast<std::uint8_t>(reason));
  return ProcessResult{Ack{seq, cmd.client_order_id, cmd.order_id, false, reason}, {ev}, {}};
}

void Exchange::account_for_md(const std::vector<MdEvent>& md_events) {
  for (const auto& ev : md_events) {
    hash_combine(md_checksum_, ev.seq);
    hash_combine(md_checksum_, static_cast<std::uint8_t>(ev.type));
    hash_combine(md_checksum_, ev.symbol_id);
    hash_combine(md_checksum_, ev.order_id);
    hash_combine(md_checksum_, ev.new_order_id);
    hash_combine(md_checksum_, ev.account_id);
    hash_combine(md_checksum_, static_cast<std::uint8_t>(ev.side));
    hash_combine(md_checksum_, static_cast<std::uint64_t>(ev.price));
    hash_combine(md_checksum_, ev.qty);
    hash_combine(md_checksum_, static_cast<std::uint8_t>(ev.reason));
    risk_.on_market_data(ev, books_.contains_order(ev.order_id));
  }
}

ProcessResult Exchange::process_frame(std::span<const std::byte> order_entry_frame, TimestampNs now_ns) {
  auto decoded = protocol::decode_client_command(order_entry_frame);
  if (!decoded.command) {
    ClientCommand dummy;
    dummy.client_order_id = 0;
    dummy.order_id = 0;
    dummy.symbol_id = 1;
    const SeqNo seq = next_seq_++;
    return reject(seq, dummy, decoded.error ? decoded.error->reason : RejectReason::Malformed);
  }
  return process_command(*decoded.command, now_ns);
}

ProcessResult Exchange::process_command(const ClientCommand& cmd, TimestampNs now_ns) {
  const SeqNo seq = next_seq_++;
  SequencedCommand sc{seq, now_ns, cmd};
  return apply_sequenced(sc, true);
}

ProcessResult Exchange::apply_sequenced(const SequencedCommand& sc, bool write_journal) {
  if (sc.seq != next_seq_ - 1 && sc.seq >= next_seq_) next_seq_ = sc.seq + 1;
  if (write_journal && journal_) journal_->append(sc);

  auto risk_result = risk_.check(sc.cmd, books_, sc.recv_ts_ns);
  if (risk_result != RejectReason::None) return reject(sc.seq, sc.cmd, risk_result);

  ProcessResult pr;
  pr.ack = Ack{sc.seq, sc.cmd.client_order_id, sc.cmd.order_id, true, RejectReason::None};

  MatchOutput out;
  switch (sc.cmd.type) {
    case CommandType::New:
      out = books_.add_order(sc.seq, sc.cmd);
      break;
    case CommandType::Cancel: {
      auto ev = books_.cancel_order(sc.seq, sc.cmd.cancel_order_id);
      if (!ev) return reject(sc.seq, sc.cmd, RejectReason::UnknownOrder);
      out.md_events.push_back(*ev);
      break;
    }
    case CommandType::Replace:
      out = books_.replace_order(sc.seq, sc.cmd);
      break;
  }
  if (out.reject != RejectReason::None) return reject(sc.seq, sc.cmd, out.reject);

  pr.md_events = std::move(out.md_events);
  for (auto& ev : pr.md_events) ev.seq = next_md_seq_++;
  pr.trades = std::move(out.trades);
  account_for_md(pr.md_events);
  return pr;
}

void Exchange::flush_journal() { if (journal_) journal_->flush(); }

std::uint64_t Exchange::checksum() const {
  std::uint64_t h = 0x45584348534d4348ull;
  hash_combine(h, books_.checksum());
  hash_combine(h, risk_.checksum());
  hash_combine(h, md_checksum_);
  return h;
}

ReplayResult replay_journal(const std::string& path, const ExchangeConfig& cfg_without_journal) {
  ExchangeConfig cfg;
  cfg.max_symbol = cfg_without_journal.max_symbol;
  cfg.ladder = cfg_without_journal.ladder;
  cfg.risk_limits = cfg_without_journal.risk_limits;
  cfg.journal_path.reset();
  Exchange ex(cfg);
  journal::JournalReader rd(path);
  if (rd.error()) return ReplayResult{0, 0, 0, rd.error()->message};
  std::size_t records = 0;
  while (auto sc = rd.next()) {
    ex.apply_sequenced(*sc, false);
    ++records;
  }
  if (rd.error()) return ReplayResult{0, 0, records, rd.error()->message};
  return ReplayResult{ex.books().checksum(), ex.md_checksum(), records, std::nullopt};
}

} // namespace exsim::core
