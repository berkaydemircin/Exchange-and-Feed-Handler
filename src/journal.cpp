#include "exsim/journal/journal.hpp"
#include "exsim/util/checksum.hpp"
#include "exsim/util/codec.hpp"

#include <filesystem>
#include <iostream>

namespace exsim::journal {
namespace {
constexpr std::uint32_t kJournalMagic = 0x4a4e4c31u; // JNL1
constexpr std::uint32_t kRecordMagic = 0x52454331u;  // REC1
} // namespace

std::vector<std::byte> encode_sequenced_command_payload(const SequencedCommand& sc) {
  std::vector<std::byte> payload;
  payload.reserve(8 + 8 + 1 + 8 + 8 + 8 + 4 + 4 + 1 + 8 + 4 + 1 + 8);
  Writer w(payload);
  w.pod(sc.seq);
  w.pod(sc.recv_ts_ns);
  w.pod(static_cast<std::uint8_t>(sc.cmd.type));
  w.pod(sc.cmd.client_order_id);
  w.pod(sc.cmd.order_id);
  w.pod(sc.cmd.cancel_order_id);
  w.pod(sc.cmd.account_id);
  w.pod(sc.cmd.symbol_id);
  w.pod(static_cast<std::uint8_t>(sc.cmd.side));
  w.pod(sc.cmd.price);
  w.pod(sc.cmd.qty);
  w.pod(static_cast<std::uint8_t>(sc.cmd.tif));
  w.pod(sc.cmd.client_ts_ns);
  return payload;
}

std::optional<SequencedCommand> decode_sequenced_command_payload(std::span<const std::byte> payload) {
  Reader r(payload);
  SequencedCommand sc;
  auto seq = r.pod<SeqNo>();
  auto recv_ts = r.pod<TimestampNs>();
  auto type = r.pod<std::uint8_t>();
  auto client_id = r.pod<ClientOrderId>();
  auto order_id = r.pod<OrderId>();
  auto cancel_id = r.pod<OrderId>();
  auto account = r.pod<AccountId>();
  auto symbol = r.pod<SymbolId>();
  auto side = r.pod<std::uint8_t>();
  auto price = r.pod<Price>();
  auto qty = r.pod<Qty>();
  auto tif = r.pod<std::uint8_t>();
  auto client_ts = r.pod<TimestampNs>();
  if (!seq || !recv_ts || !type || !client_id || !order_id || !cancel_id || !account || !symbol || !side || !price || !qty || !tif || !client_ts || r.remaining() != 0) return std::nullopt;
  sc.seq = *seq;
  sc.recv_ts_ns = *recv_ts;
  sc.cmd.type = static_cast<CommandType>(*type);
  sc.cmd.client_order_id = *client_id;
  sc.cmd.order_id = *order_id;
  sc.cmd.cancel_order_id = *cancel_id;
  sc.cmd.account_id = *account;
  sc.cmd.symbol_id = *symbol;
  sc.cmd.side = static_cast<Side>(*side);
  sc.cmd.price = *price;
  sc.cmd.qty = *qty;
  sc.cmd.tif = static_cast<TimeInForce>(*tif);
  sc.cmd.client_ts_ns = *client_ts;
  return sc;
}

JournalWriter::JournalWriter(const std::string& path) : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
  Writer w(scratch_);
  w.pod(kJournalMagic);
  w.pod(kProtocolVersion);
  out_.write(reinterpret_cast<const char*>(scratch_.data()), static_cast<std::streamsize>(scratch_.size()));
  scratch_.clear();
}

JournalWriter::~JournalWriter() { flush(); }

void JournalWriter::append(const SequencedCommand& cmd) {
  auto payload = encode_sequenced_command_payload(cmd);
  scratch_.clear();
  Writer w(scratch_);
  w.pod(kRecordMagic);
  w.pod(static_cast<std::uint32_t>(payload.size()));
  w.pod(fnv1a64(payload));
  w.bytes(payload);
  out_.write(reinterpret_cast<const char*>(scratch_.data()), static_cast<std::streamsize>(scratch_.size()));
}

void JournalWriter::flush() { if (out_.is_open()) out_.flush(); }

JournalReader::JournalReader(const std::string& path) : in_(path, std::ios::binary) {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  if (!in_.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || !in_.read(reinterpret_cast<char*>(&version), sizeof(version))) {
    error_ = JournalError{"failed to read journal header"};
    return;
  }
  if (magic != kJournalMagic || version != kProtocolVersion) {
    error_ = JournalError{"bad journal header"};
  }
}

std::optional<SequencedCommand> JournalReader::next() {
  if (error_) return std::nullopt;
  std::uint32_t magic = 0;
  std::uint32_t len = 0;
  std::uint64_t checksum = 0;
  if (!in_.read(reinterpret_cast<char*>(&magic), sizeof(magic))) {
    if (in_.eof()) return std::nullopt;
    error_ = JournalError{"failed reading record magic"};
    return std::nullopt;
  }
  if (!in_.read(reinterpret_cast<char*>(&len), sizeof(len)) || !in_.read(reinterpret_cast<char*>(&checksum), sizeof(checksum))) {
    error_ = JournalError{"short journal record"};
    return std::nullopt;
  }
  if (magic != kRecordMagic) {
    error_ = JournalError{"bad record magic"};
    return std::nullopt;
  }
  std::vector<std::byte> payload(len);
  if (!in_.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()))) {
    error_ = JournalError{"short record payload"};
    return std::nullopt;
  }
  if (fnv1a64(payload) != checksum) {
    error_ = JournalError{"record checksum mismatch"};
    return std::nullopt;
  }
  auto sc = decode_sequenced_command_payload(payload);
  if (!sc) error_ = JournalError{"bad sequenced command payload"};
  return sc;
}

} // namespace exsim::journal
