#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace dbdiff {

class Sha256 {
public:
  Sha256();
  ~Sha256();
  Sha256(Sha256&&) noexcept;
  Sha256& operator=(Sha256&&) noexcept;
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void add(std::span<const std::byte> bytes);
  void add(std::string_view value);
  void add_length_prefixed(std::string_view value);
  [[nodiscard]] std::string finish_hex();

private:
  struct State;
  std::unique_ptr<State> state_;
};

[[nodiscard]] std::string sha256_hex(std::string_view value);

} // namespace dbdiff
