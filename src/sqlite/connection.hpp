#pragma once

#include "dbdiff/error.hpp"
#include "dbdiff/sqlite.hpp"
#include <chrono>
#include <climits>
#include <cstddef>
#include <exception>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace dbdiff::sqlite::detail {
[[nodiscard]] std::string sqlite_error_message(sqlite3*, int, std::string_view);
[[noreturn]] void throw_sqlite(sqlite3*, int, std::string_view);
void execute_one(sqlite3*, std::string_view, bool reject_result_rows = false);
[[nodiscard]] int query_integer(sqlite3*, std::string_view);
void execute_cleanup(sqlite3*, const char*) noexcept;
void rollback_noexcept(sqlite3*) noexcept;
void restore_foreign_keys(sqlite3*, int);
void restore_foreign_keys_noexcept(sqlite3*, int) noexcept;
void copy_database(sqlite3*, sqlite3*);
void execute_unit_plain(sqlite3*, const ParsedScript&, const ExecutionUnit&);
class Statement final {
public:
  Statement(sqlite3* database, const std::string_view sql) : database_{database} {
    if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
      throw Error{ErrorCode::database, "SQLite statement exceeds the supported size"};
    }

    const char* tail = nullptr;
    const auto result = sqlite3_prepare_v3(database_, sql.data(), static_cast<int>(sql.size()), 0,
                                           &statement_, &tail);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "preparing statement");
    }
    consumed_ = static_cast<std::size_t>(tail - sql.data());
  }

  ~Statement() {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  [[nodiscard]] bool valid() const noexcept { return statement_ != nullptr; }
  [[nodiscard]] std::size_t consumed() const noexcept { return consumed_; }

  void bind_text(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
      throw Error{ErrorCode::database, "SQLite bound value exceeds the supported size"};
    }
    const auto result = sqlite3_bind_text(statement_, index, value.data(),
                                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "binding statement value");
    }
  }

  void bind_blob(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
      throw Error{ErrorCode::database, "SQLite bound value exceeds the supported size"};
    }
    const auto result = sqlite3_bind_blob(statement_, index, value.data(),
                                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "binding statement value");
    }
  }

  void bind_integer(const int index, const sqlite3_int64 value) {
    const auto result = sqlite3_bind_int64(statement_, index, value);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "binding statement value");
    }
  }

  [[nodiscard]] int step() {
    const auto result = sqlite3_step(statement_);
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
      throw_sqlite(database_, result, "executing statement");
    }
    return result;
  }

  [[nodiscard]] int integer(const int column) const {
    return sqlite3_column_int(statement_, column);
  }

  [[nodiscard]] sqlite3_int64 integer64(const int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] std::optional<std::string> optional_text(const int column) const {
    if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
      return std::nullopt;
    }
    const auto* value = sqlite3_column_text(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    if (value == nullptr || size < 0) {
      throw Error{ErrorCode::database, "SQLite returned an invalid text column"};
    }
    return std::string{reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
  }

  [[nodiscard]] std::string text(const int column) const {
    auto value = optional_text(column);
    if (!value.has_value()) {
      throw Error{ErrorCode::database, "SQLite returned NULL for a required text column"};
    }
    return std::move(*value);
  }

  [[nodiscard]] std::string blob(const int column) const {
    if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
      throw Error{ErrorCode::database, "SQLite returned NULL for a required blob column"};
    }
    const auto* value = sqlite3_column_blob(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    if ((value == nullptr && size != 0) || size < 0) {
      throw Error{ErrorCode::database, "SQLite returned an invalid blob column"};
    }
    if (size == 0) {
      return {};
    }
    return std::string{static_cast<const char*>(value), static_cast<std::size_t>(size)};
  }

private:
  sqlite3* database_{nullptr};
  sqlite3_stmt* statement_{nullptr};
  std::size_t consumed_{0};
};

// SQLite resets deferral whenever an implicit or explicit transaction ends.
// Internal reads and bookkeeping must not consume a migration's session state.
class DeferredForeignKeysGuard final {
public:
  explicit DeferredForeignKeysGuard(sqlite3* database)
      : database_{database}, enabled_{query_integer(database, "PRAGMA defer_foreign_keys;")},
        transaction_active_{sqlite3_get_autocommit(database) == 0},
        exceptions_{std::uncaught_exceptions()} {}
  ~DeferredForeignKeysGuard() noexcept(false) {
    // Internal reads cannot end an active transaction. Reissuing even an
    // unchanged OFF pragma there would clear deferred constraint violations.
    // If a read aborted the transaction, retain SQLite's rollback reset too.
    if (transaction_active_ || sqlite3_get_autocommit(database_) == 0) {
      return;
    }
    const auto* sql =
        enabled_ != 0 ? "PRAGMA defer_foreign_keys=ON;" : "PRAGMA defer_foreign_keys=OFF;";
    if (std::uncaught_exceptions() > exceptions_) {
      execute_cleanup(database_, sql);
    } else {
      execute_one(database_, sql);
    }
  }
  DeferredForeignKeysGuard(const DeferredForeignKeysGuard&) = delete;
  DeferredForeignKeysGuard& operator=(const DeferredForeignKeysGuard&) = delete;

private:
  sqlite3* database_;
  int enabled_;
  bool transaction_active_;
  int exceptions_;
};

struct ProgressState {
  std::chrono::milliseconds timeout;
  std::chrono::steady_clock::time_point deadline{};
  bool active{false};
};

class OperationDeadline final {
public:
  explicit OperationDeadline(ProgressState& state) : state_{state} {
    state_.deadline = std::chrono::steady_clock::now() + state_.timeout;
    state_.active = true;
  }

  OperationDeadline(const OperationDeadline&) = delete;
  OperationDeadline& operator=(const OperationDeadline&) = delete;

  ~OperationDeadline() { state_.active = false; }

private:
  ProgressState& state_;
};

void configure_database(sqlite3*, ProgressState&, const ConnectionSettings&);
[[nodiscard]] int open_flags(OpenMode);
void validate_database_path(const std::filesystem::path&);
} // namespace dbdiff::sqlite::detail
