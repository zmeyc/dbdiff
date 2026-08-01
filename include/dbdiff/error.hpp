#pragma once

#include <stdexcept>
#include <string>

namespace dbdiff {

enum class ErrorCode {
  configuration,
  source,
  migration,
  database,
  unsupported,
  drift,
  execution,
};

class Error : public std::runtime_error {
public:
  Error(ErrorCode code, const std::string& message);

  [[nodiscard]] ErrorCode code() const noexcept;

private:
  ErrorCode code_;
};

} // namespace dbdiff
