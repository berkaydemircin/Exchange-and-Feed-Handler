#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace exsim {

inline std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept {
  std::uint64_t h = 1469598103934665603ull;
  for (std::byte b : data) {
    h ^= static_cast<std::uint8_t>(b);
    h *= 1099511628211ull;
  }
  return h;
}

inline std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

inline void hash_combine(std::uint64_t& h, std::uint64_t v) noexcept {
  h ^= mix64(v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
}

} // namespace exsim
