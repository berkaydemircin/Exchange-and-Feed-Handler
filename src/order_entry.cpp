#include "exsim/protocol/order_entry.hpp"
#include "exsim/util/checksum.hpp"
#include "exsim/util/codec.hpp"

#include <cstring>

namespace exsim::protocol {
namespace {
constexpr std::uint16_t kFrameHeaderBytes = 4 + 2 + 2 + 8; // magic, version, len, checksum
constexpr std::uint16_t kPayloadBytes = static_cast<std::uint16_t>(kClientCommandPayloadBytes);

std::optional<DecodeError> fail(RejectReason r, std::string msg) {
  return DecodeError{r, std::move(msg)};
}

template <class T>
void put(std::array<std::byte, kClientCommandFrameBytes>& out, std::size_t& pos, T value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(out.data() + pos, &value, sizeof(T));
  pos += sizeof(T);
}
} // namespace

Bytes encode_client_command(const ClientCommand& cmd) {
  Bytes payload;
  payload.reserve(kPayloadBytes);
  Writer pw(payload);
  pw.pod(static_cast<std::uint8_t>(cmd.type));
  pw.pod(cmd.client_order_id);
  pw.pod(cmd.order_id);
  pw.pod(cmd.cancel_order_id);
  pw.pod(cmd.account_id);
  pw.pod(cmd.symbol_id);
  pw.pod(static_cast<std::uint8_t>(cmd.side));
  pw.pod(cmd.price);
  pw.pod(cmd.qty);
  pw.pod(static_cast<std::uint8_t>(cmd.tif));
  pw.pod(cmd.client_ts_ns);

  Bytes frame;
  frame.reserve(kFrameHeaderBytes + payload.size());
  Writer w(frame);
  w.pod(kProtocolMagic);
  w.pod(kProtocolVersion);
  w.pod(static_cast<std::uint16_t>(payload.size()));
  w.pod(fnv1a64(payload));
  w.bytes(payload);
  return frame;
}

ClientCommandFrame encode_client_command_frame(const ClientCommand& cmd) {
  ClientCommandFrame frame;
  std::size_t pos = kFrameHeaderBytes;
  put(frame.bytes, pos, static_cast<std::uint8_t>(cmd.type));
  put(frame.bytes, pos, cmd.client_order_id);
  put(frame.bytes, pos, cmd.order_id);
  put(frame.bytes, pos, cmd.cancel_order_id);
  put(frame.bytes, pos, cmd.account_id);
  put(frame.bytes, pos, cmd.symbol_id);
  put(frame.bytes, pos, static_cast<std::uint8_t>(cmd.side));
  put(frame.bytes, pos, cmd.price);
  put(frame.bytes, pos, cmd.qty);
  put(frame.bytes, pos, static_cast<std::uint8_t>(cmd.tif));
  put(frame.bytes, pos, cmd.client_ts_ns);

  const std::span<const std::byte> payload(frame.bytes.data() + kFrameHeaderBytes, kPayloadBytes);
  const std::uint64_t checksum = fnv1a64(payload);
  pos = 0;
  put(frame.bytes, pos, kProtocolMagic);
  put(frame.bytes, pos, kProtocolVersion);
  put(frame.bytes, pos, kPayloadBytes);
  put(frame.bytes, pos, checksum);
  frame.size = kClientCommandFrameBytes;
  return frame;
}

DecodeResult decode_client_command(std::span<const std::byte> frame) {
  Reader r(frame);
  auto magic = r.pod<std::uint32_t>();
  auto version = r.pod<std::uint16_t>();
  auto len = r.pod<std::uint16_t>();
  auto checksum = r.pod<std::uint64_t>();
  if (!magic || !version || !len || !checksum) return {std::nullopt, fail(RejectReason::Malformed, "short frame header")};
  if (*magic != kProtocolMagic) return {std::nullopt, fail(RejectReason::Malformed, "bad magic")};
  if (*version != kProtocolVersion) return {std::nullopt, fail(RejectReason::Malformed, "unsupported version")};
  auto payload = r.bytes(*len);
  if (!payload || r.remaining() != 0) return {std::nullopt, fail(RejectReason::Malformed, "bad frame length")};
  if (fnv1a64(*payload) != *checksum) return {std::nullopt, fail(RejectReason::Malformed, "checksum mismatch")};

  Reader pr(*payload);
  auto type = pr.pod<std::uint8_t>();
  auto client_id = pr.pod<ClientOrderId>();
  auto order_id = pr.pod<OrderId>();
  auto cancel_id = pr.pod<OrderId>();
  auto account = pr.pod<AccountId>();
  auto symbol = pr.pod<SymbolId>();
  auto side = pr.pod<std::uint8_t>();
  auto price = pr.pod<Price>();
  auto qty = pr.pod<Qty>();
  auto tif = pr.pod<std::uint8_t>();
  auto ts = pr.pod<TimestampNs>();
  if (!type || !client_id || !order_id || !cancel_id || !account || !symbol || !side || !price || !qty || !tif || !ts || pr.remaining() != 0) {
    return {std::nullopt, fail(RejectReason::Malformed, "bad payload")};
  }

  ClientCommand cmd;
  cmd.type = static_cast<CommandType>(*type);
  cmd.client_order_id = *client_id;
  cmd.order_id = *order_id;
  cmd.cancel_order_id = *cancel_id;
  cmd.account_id = *account;
  cmd.symbol_id = *symbol;
  cmd.side = static_cast<Side>(*side);
  cmd.price = *price;
  cmd.qty = *qty;
  cmd.tif = static_cast<TimeInForce>(*tif);
  cmd.client_ts_ns = *ts;

  auto vr = validate_client_command(cmd);
  if (vr != RejectReason::None) return {std::nullopt, fail(vr, "semantic validation failed")};
  return {cmd, std::nullopt};
}

RejectReason validate_client_command(const ClientCommand& cmd) noexcept {
  if (cmd.account_id == 0) return RejectReason::Malformed;
  if (cmd.symbol_id == 0) return RejectReason::InvalidSymbol;
  if (cmd.type != CommandType::New && cmd.type != CommandType::Cancel && cmd.type != CommandType::Replace) return RejectReason::Malformed;
  if (cmd.side != Side::Buy && cmd.side != Side::Sell) return RejectReason::Malformed;
  if (cmd.tif != TimeInForce::Gtc && cmd.tif != TimeInForce::Ioc) return RejectReason::Malformed;
  if (cmd.type == CommandType::New) {
    if (cmd.order_id == kInvalidOrderId) return RejectReason::Malformed;
    if (cmd.qty == 0) return RejectReason::InvalidQty;
    if (cmd.price <= 0) return RejectReason::InvalidPrice;
  }
  if (cmd.type == CommandType::Cancel) {
    if (cmd.cancel_order_id == kInvalidOrderId) return RejectReason::UnknownOrder;
  }
  if (cmd.type == CommandType::Replace) {
    if (cmd.cancel_order_id == kInvalidOrderId || cmd.order_id == kInvalidOrderId) return RejectReason::UnknownOrder;
    if (cmd.qty == 0) return RejectReason::InvalidQty;
    if (cmd.price <= 0) return RejectReason::InvalidPrice;
  }
  return RejectReason::None;
}

} // namespace exsim::protocol
