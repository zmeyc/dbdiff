#include "dbdiff/error.hpp"

namespace dbdiff {

Error::Error(const ErrorCode code, const std::string& message)
    : std::runtime_error{message}, code_{code} {}

ErrorCode Error::code() const noexcept { return code_; }

} // namespace dbdiff
