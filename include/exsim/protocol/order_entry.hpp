#pragma once

#include "exsim/types.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace exsim::protocol {

constexpr std::size_t kClientCommandPayloadBytes = 1 + 8 + 8 + 8 + 4 + 4 + 1 + 8 + 4 + 1 + 8;
constexpr std::size_t kClientCommandFrameBytes = 4 + 2 + 2 + 8 + kClientCommandPayloadBytes;

struct ClientCommandFrame {
  std::array<std::byte, kClientCommandFrameBytes> bytes{};
  std::size_t size{kClientCommandFrameBytes};
  [[nodiscard]] std::span<const std::byte> span() const noexcept { return std::span<const std::byte>(bytes.data(), size); }
};

struct DecodeError {
  RejectReason reason{RejectReason::Malformed};
  std::string message;
};

struct DecodeResult {
  std::optional<ClientCommand> command;
  std::optional<DecodeError> error;
};

Bytes encode_client_command(const ClientCommand& cmd);
ClientCommandFrame encode_client_command_frame(const ClientCommand& cmd);
DecodeResult decode_client_command(std::span<const std::byte> frame);
RejectReason validate_client_command(const ClientCommand& cmd) noexcept;

} // namespace exsim::protocol
