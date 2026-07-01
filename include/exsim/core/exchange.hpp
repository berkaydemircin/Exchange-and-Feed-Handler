#pragma once

#include "exsim/core/order_book.hpp"
#include "exsim/core/risk.hpp"
#include "exsim/journal/journal.hpp"
#include "exsim/types.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace exsim::core {

struct ProcessResult {
  Ack ack;
  std::vector<MdEvent> md_events;
  std::vector<Trade> trades;
};

struct ExchangeConfig {
  SymbolId max_symbol{1000};
  PriceLadderConfig ladder{};
  RiskLimits risk_limits{};
  std::optional<std::string> journal_path;
};

class Exchange {
 public:
  explicit Exchange(ExchangeConfig cfg);

  ProcessResult process_frame(std::span<const std::byte> order_entry_frame, TimestampNs now_ns);
  ProcessResult process_command(const ClientCommand& cmd, TimestampNs now_ns);
  ProcessResult apply_sequenced(const SequencedCommand& sc, bool write_journal);

  void flush_journal();
  [[nodiscard]] SeqNo next_seq() const noexcept { return next_seq_; }
  [[nodiscard]] const BookManager& books() const noexcept { return books_; }
  [[nodiscard]] std::uint64_t checksum() const;
  [[nodiscard]] std::uint64_t md_checksum() const noexcept { return md_checksum_; }
  [[nodiscard]] std::size_t live_orders() const { return books_.live_orders(); }

 private:
  SeqNo next_seq_{1};
  SeqNo next_md_seq_{1};
  PriceLadderConfig ladder_{};
  BookManager books_;
  RiskEngine risk_;
  std::unique_ptr<journal::JournalWriter> journal_;
  std::uint64_t md_checksum_{0x4d4443484b53554dull};

  ProcessResult reject(SeqNo seq, const ClientCommand& cmd, RejectReason reason);
  void account_for_md(const std::vector<MdEvent>& md_events);
};

struct ReplayResult {
  std::uint64_t book_checksum{0};
  std::uint64_t md_checksum{0};
  std::size_t records{0};
  std::optional<std::string> error;
};

ReplayResult replay_journal(const std::string& path, const ExchangeConfig& cfg_without_journal);

} // namespace exsim::core
