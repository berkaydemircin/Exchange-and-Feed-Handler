#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace exsim {

class Writer {
 public:
  explicit Writer(std::vector<std::byte>& out) : out_(out) {}

  template <class T>
  void pod(T v) {
    static_assert(std::is_trivially_copyable_v<T>);
    if constexpr (std::endian::native == std::endian::big) {
      throw std::runtime_error("big-endian host not supported");
    }
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out_.insert(out_.end(), p, p + sizeof(T));
  }

  void bytes(std::span<const std::byte> s) { out_.insert(out_.end(), s.begin(), s.end()); }

 private:
  std::vector<std::byte>& out_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> in) : in_(in) {}

  template <class T>
  std::optional<T> pod() {
    static_assert(std::is_trivially_copyable_v<T>);
    if (pos_ + sizeof(T) > in_.size()) return std::nullopt;
    T v{};
    std::memcpy(&v, in_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  std::optional<std::span<const std::byte>> bytes(std::size_t n) {
    if (pos_ + n > in_.size()) return std::nullopt;
    auto s = in_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

  [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

 private:
  std::span<const std::byte> in_;
  std::size_t pos_{0};
};

} // namespace exsim
