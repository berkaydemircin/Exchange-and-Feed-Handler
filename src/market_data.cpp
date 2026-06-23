#include "exsim/md/market_data.hpp"
#include "exsim/util/checksum.hpp"
#include "exsim/util/codec.hpp"

#include <cstring>
#include <type_traits>

namespace exsim::md {
namespace {
constexpr std::uint32_t kMdMagic = 0x4d445349u; // MDSI
constexpr std::uint16_t kMdVersion = 1;
constexpr std::uint16_t kPayloadBytes = static_cast<std::uint16_t>(kMdPayloadBytes);
constexpr std::uint16_t kFrameHeaderBytes = 4 + 2 + 2 + 8;

DecodeResult fail(std::string msg, SeqNo expected = 0, SeqNo actual = 0) {
  return {std::nullopt, DecodeError{std::move(msg), expected, actual}};
}

template <class T>
void put(std::array<std::byte, kMdFrameBytes>& out, std::size_t& pos, T value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(out.data() + pos, &value, sizeof(T));
  pos += sizeof(T);
}
} // namespace

Bytes encode_md_event(const MdEvent& ev) {
  Bytes payload;
  payload.reserve(kPayloadBytes);
  Writer p(payload);
  p.pod(ev.seq);
  p.pod(static_cast<std::uint8_t>(ev.type));
  p.pod(ev.symbol_id);
  p.pod(ev.order_id);
  p.pod(ev.new_order_id);
  p.pod(ev.account_id);
  p.pod(static_cast<std::uint8_t>(ev.side));
  p.pod(ev.price);
  p.pod(ev.qty);
  p.pod(static_cast<std::uint8_t>(ev.reason));

  Bytes frame;
  frame.reserve(4 + 2 + 2 + 8 + payload.size());
  Writer w(frame);
  w.pod(kMdMagic);
  w.pod(kMdVersion);
  w.pod(static_cast<std::uint16_t>(payload.size()));
  w.pod(fnv1a64(payload));
  w.bytes(payload);
  return frame;
}

MdFrame encode_md_event_frame(const MdEvent& ev) {
  MdFrame frame;
  std::size_t pos = kFrameHeaderBytes;
  put(frame.bytes, pos, ev.seq);
  put(frame.bytes, pos, static_cast<std::uint8_t>(ev.type));
  put(frame.bytes, pos, ev.symbol_id);
  put(frame.bytes, pos, ev.order_id);
  put(frame.bytes, pos, ev.new_order_id);
  put(frame.bytes, pos, ev.account_id);
  put(frame.bytes, pos, static_cast<std::uint8_t>(ev.side));
  put(frame.bytes, pos, ev.price);
  put(frame.bytes, pos, ev.qty);
  put(frame.bytes, pos, static_cast<std::uint8_t>(ev.reason));

  const std::span<const std::byte> payload(frame.bytes.data() + kFrameHeaderBytes, kPayloadBytes);
  const std::uint64_t checksum = fnv1a64(payload);
  pos = 0;
  put(frame.bytes, pos, kMdMagic);
  put(frame.bytes, pos, kMdVersion);
  put(frame.bytes, pos, kPayloadBytes);
  put(frame.bytes, pos, checksum);
  frame.size = kMdFrameBytes;
  return frame;
}

DecodeResult decode_md_event(std::span<const std::byte> frame, std::optional<SeqNo> expected_seq) {
  Reader r(frame);
  auto magic = r.pod<std::uint32_t>();
  auto version = r.pod<std::uint16_t>();
  auto len = r.pod<std::uint16_t>();
  auto checksum = r.pod<std::uint64_t>();
  if (!magic || !version || !len || !checksum) return fail("short md frame header");
  if (*magic != kMdMagic) return fail("bad md magic");
  if (*version != kMdVersion) return fail("bad md version");
  auto payload = r.bytes(*len);
  if (!payload || r.remaining() != 0) return fail("bad md frame length");
  if (fnv1a64(*payload) != *checksum) return fail("md checksum mismatch");

  Reader pr(*payload);
  auto seq = pr.pod<SeqNo>();
  auto type = pr.pod<std::uint8_t>();
  auto symbol = pr.pod<SymbolId>();
  auto order_id = pr.pod<OrderId>();
  auto new_order_id = pr.pod<OrderId>();
  auto account = pr.pod<AccountId>();
  auto side = pr.pod<std::uint8_t>();
  auto price = pr.pod<Price>();
  auto qty = pr.pod<Qty>();
  auto reason = pr.pod<std::uint8_t>();
  if (!seq || !type || !symbol || !order_id || !new_order_id || !account || !side || !price || !qty || !reason || pr.remaining() != 0) return fail("bad md payload");
  if (expected_seq && *seq != *expected_seq) return fail("feed gap", *expected_seq, *seq);

  MdEvent ev;
  ev.seq = *seq;
  ev.type = static_cast<MdType>(*type);
  ev.symbol_id = *symbol;
  ev.order_id = *order_id;
  ev.new_order_id = *new_order_id;
  ev.account_id = *account;
  ev.side = static_cast<Side>(*side);
  ev.price = *price;
  ev.qty = *qty;
  ev.reason = static_cast<RejectReason>(*reason);
  return {ev, std::nullopt};
}

DecodeResult FeedDecoder::decode(std::span<const std::byte> frame) {
  auto res = decode_md_event(frame, expected_);
  if (res.event) ++expected_;
  return res;
}

} // namespace exsim::md
