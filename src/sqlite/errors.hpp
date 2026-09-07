#pragma once
#include "dbdiff/error.hpp"
#include <string>
namespace dbdiff::sqlite::detail {
[[noreturn]] inline void throw_migration(const std::string& message) {
  throw Error{ErrorCode::migration, message};
}

[[noreturn]] inline void throw_unsupported(const std::string& message) {
  throw Error{ErrorCode::unsupported, message};
}

[[noreturn]] inline void throw_history_corruption(const std::string& message) {
  throw Error{ErrorCode::database, "invalid dbdiff SQLite migration history: " + message};
}

} // namespace dbdiff::sqlite::detail
