#pragma once

#include "exsim/types.hpp"
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace exsim::journal {

struct JournalError {
  std::string message;
};

class JournalWriter {
 public:
  explicit JournalWriter(const std::string& path);
  ~JournalWriter();
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;

  bool good() const noexcept { return out_.good(); }
  void append(const SequencedCommand& cmd);
  void flush();
  const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
  std::ofstream out_;
  std::vector<std::byte> scratch_;
};

class JournalReader {
 public:
  explicit JournalReader(const std::string& path);
  std::optional<SequencedCommand> next();
  std::optional<JournalError> error() const { return error_; }
 private:
  std::ifstream in_;
  std::optional<JournalError> error_;
};

std::vector<std::byte> encode_sequenced_command_payload(const SequencedCommand& cmd);
std::optional<SequencedCommand> decode_sequenced_command_payload(std::span<const std::byte> payload);

} // namespace exsim::journal
