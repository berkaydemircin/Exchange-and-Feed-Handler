#pragma once

#include "exsim/types.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace exsim::md {

struct DecodeError {
  std::string message;
  SeqNo expected{0};
  SeqNo actual{0};
};

struct DecodeResult {
  std::optional<MdEvent> event;
  std::optional<DecodeError> error;
};

constexpr std::size_t kMdPayloadBytes = 8 + 1 + 4 + 8 + 8 + 4 + 1 + 8 + 4 + 1;
constexpr std::size_t kMdFrameBytes = 4 + 2 + 2 + 8 + kMdPayloadBytes;

struct MdFrame {
  std::array<std::byte, kMdFrameBytes> bytes{};
  std::size_t size{kMdFrameBytes};
  [[nodiscard]] std::span<const std::byte> span() const noexcept { return std::span<const std::byte>(bytes.data(), size); }
};

Bytes encode_md_event(const MdEvent& ev);
MdFrame encode_md_event_frame(const MdEvent& ev);
DecodeResult decode_md_event(std::span<const std::byte> frame, std::optional<SeqNo> expected_seq = std::nullopt);

class FeedDecoder {
 public:
  explicit FeedDecoder(SeqNo first_expected = 1) : expected_(first_expected) {}
  DecodeResult decode(std::span<const std::byte> frame);
  [[nodiscard]] SeqNo expected() const noexcept { return expected_; }
  void reset(SeqNo next_expected) noexcept { expected_ = next_expected; }
 private:
  SeqNo expected_;
};

} // namespace exsim::md
