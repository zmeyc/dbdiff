#include "dbdiff/hash.hpp"

#include "dbdiff/error.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <utility>

namespace dbdiff {

struct Sha256::State {
  State() : context{EVP_MD_CTX_new(), &EVP_MD_CTX_free} {
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
      throw Error{ErrorCode::execution, "failed to initialize SHA-256"};
    }
  }

  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context;
  bool finished{false};
};

Sha256::Sha256() : state_{std::make_unique<State>()} {}
Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::add(const std::span<const std::byte> bytes) {
  if (state_->finished) {
    throw Error{ErrorCode::execution, "cannot append to a finished SHA-256 digest"};
  }
  if (!bytes.empty() && EVP_DigestUpdate(state_->context.get(), bytes.data(), bytes.size()) != 1) {
    throw Error{ErrorCode::execution, "failed to update SHA-256"};
  }
}

void Sha256::add(const std::string_view value) {
  add(std::as_bytes(std::span{value.data(), value.size()}));
}

void Sha256::add_length_prefixed(const std::string_view value) {
  const auto length = static_cast<std::uint64_t>(value.size());
  std::array<std::byte, sizeof(length)> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const auto shift = static_cast<unsigned>((encoded.size() - index - 1U) * 8U);
    encoded[index] = static_cast<std::byte>((length >> shift) & 0xffU);
  }
  add(encoded);
  add(value);
}

std::string Sha256::finish_hex() {
  if (state_->finished) {
    throw Error{ErrorCode::execution, "SHA-256 digest was already finished"};
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_DigestFinal_ex(state_->context.get(), digest.data(), &size) != 1) {
    throw Error{ErrorCode::execution, "failed to finish SHA-256"};
  }
  state_->finished = true;

  std::ostringstream result;
  result << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < size; ++index) {
    result << std::setw(2) << static_cast<unsigned int>(digest[index]);
  }
  return result.str();
}

std::string sha256_hex(const std::string_view value) {
  Sha256 digest;
  digest.add(value);
  return digest.finish_hex();
}

} // namespace dbdiff
