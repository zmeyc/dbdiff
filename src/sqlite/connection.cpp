#include "connection.hpp"
#include "errors.hpp"
#include "sql.hpp"
#include <array>
#include <climits>
#include <filesystem>
#include <sstream>

namespace dbdiff::sqlite::detail {
[[nodiscard]] std::string sqlite_error_message(sqlite3* database, const int result,
                                               const std::string_view action) {
  std::ostringstream message;
  message << action << " failed (SQLite " << result << ')';
  if (database != nullptr) {
    message << ": " << sqlite3_errmsg(database);
    const auto offset = sqlite3_error_offset(database);
    if (offset >= 0) {
      message << " at SQL byte " << offset;
    }
  }
  return message.str();
}

[[noreturn]] void throw_sqlite(sqlite3* database, const int result, const std::string_view action) {
  throw Error{ErrorCode::database, sqlite_error_message(database, result, action)};
}

void require_consumed_statement(const std::string_view sql, const Statement& statement) {
  if (statement.consumed() > sql.size()) {
    throw Error{ErrorCode::database, "SQLite reported an invalid statement boundary"};
  }
  if (has_sql(sql.substr(statement.consumed()))) {
    throw Error{ErrorCode::database, "SQLite statement span contains more than one statement"};
  }
}

void execute_one(sqlite3* database, const std::string_view sql, const bool reject_result_rows) {
  Statement statement{database, sql};
  if (!statement.valid()) {
    throw Error{ErrorCode::database, "SQLite did not compile an executable statement"};
  }
  require_consumed_statement(sql, statement);

  while (true) {
    const auto result = statement.step();
    if (result == SQLITE_DONE) {
      return;
    }
    if (reject_result_rows) {
      const auto table = statement.optional_text(0).value_or("<unknown>");
      const auto row = statement.optional_text(1).value_or("<unknown>");
      std::string message{"SQLite foreign-key violation in table "};
      message.append(table);
      message.append(" at row ");
      message.append(row);
      throw Error{ErrorCode::database, message};
    }
  }
}

int deny_unsafe_schema_action(void*, const int action, const char*, const char* second_argument,
                              const char*, const char*) noexcept {
  switch (action) {
  case SQLITE_ATTACH:
  case SQLITE_DETACH:
  case SQLITE_CREATE_VTABLE:
  case SQLITE_DROP_VTABLE:
  case SQLITE_CREATE_TEMP_INDEX:
  case SQLITE_CREATE_TEMP_TABLE:
  case SQLITE_CREATE_TEMP_TRIGGER:
  case SQLITE_CREATE_TEMP_VIEW:
  case SQLITE_DROP_TEMP_INDEX:
  case SQLITE_DROP_TEMP_TABLE:
  case SQLITE_DROP_TEMP_TRIGGER:
  case SQLITE_DROP_TEMP_VIEW:
    return SQLITE_DENY;
  case SQLITE_FUNCTION:
    return second_argument != nullptr && sqlite3_stricmp(second_argument, "load_extension") == 0
               ? SQLITE_DENY
               : SQLITE_OK;
  default:
    return SQLITE_OK;
  }
}

[[nodiscard]] int query_integer(sqlite3* database, const std::string_view sql) {
  Statement statement{database, sql};
  if (!statement.valid()) {
    throw Error{ErrorCode::database, "SQLite did not compile an integer query"};
  }
  require_consumed_statement(sql, statement);
  if (statement.step() != SQLITE_ROW) {
    throw Error{ErrorCode::database, "SQLite integer query returned no row"};
  }
  const auto value = statement.integer(0);
  if (statement.step() != SQLITE_DONE) {
    throw Error{ErrorCode::database, "SQLite integer query returned more than one row"};
  }
  return value;
}

[[nodiscard]] int cancel_expired_statement(void* state) noexcept {
  auto& progress = *static_cast<ProgressState*>(state);
  if (progress.active && std::chrono::steady_clock::now() >= progress.deadline) {
    progress.active = false;
    return 1;
  }
  return 0;
}

constexpr int minimum_sqlite_version = 3'045'000;
void configure_database(sqlite3* database, ProgressState& progress,
                        const ConnectionSettings& settings) {
  if (sqlite3_libversion_number() < minimum_sqlite_version) {
    throw Error{ErrorCode::unsupported,
                "SQLite 3.45.0 or newer is required; loaded " + std::string{sqlite3_libversion()}};
  }
  if (sqlite3_compileoption_used("OMIT_FOREIGN_KEY") != 0 ||
      sqlite3_compileoption_used("OMIT_TRIGGER") != 0) {
    throw Error{ErrorCode::unsupported,
                "the loaded SQLite library omits foreign-key or trigger support"};
  }

  if (settings.lock_timeout.count() <= 0 || settings.lock_timeout.count() > INT_MAX ||
      settings.statement_timeout.count() <= 0 || settings.statement_timeout.count() > INT_MAX) {
    throw Error{ErrorCode::configuration, "SQLite connection timeouts are outside range"};
  }

  progress.timeout = settings.statement_timeout;
  sqlite3_progress_handler(database, 1000, cancel_expired_statement, &progress);

  auto result = sqlite3_extended_result_codes(database, 1);
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "enabling extended result codes");
  }
  result = sqlite3_busy_timeout(database, static_cast<int>(settings.lock_timeout.count()));
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "setting busy timeout");
  }

  const std::array database_settings{
      std::pair{SQLITE_DBCONFIG_DEFENSIVE, 1},
      std::pair{SQLITE_DBCONFIG_DQS_DDL, 0},
      std::pair{SQLITE_DBCONFIG_DQS_DML, 0},
      std::pair{SQLITE_DBCONFIG_LEGACY_ALTER_TABLE, 0},
      std::pair{SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0},
  };
  for (const auto& [setting, value] : database_settings) {
    int previous = 0;
    result = sqlite3_db_config(database, setting, value, &previous);
    if (result != SQLITE_OK) {
      throw_sqlite(database, result, "configuring SQLite connection");
    }
  }

  static_cast<void>(sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0));
  if (sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, -1) != 0) {
    throw Error{ErrorCode::unsupported, "SQLite attached databases could not be disabled"};
  }
  result = sqlite3_set_authorizer(database, deny_unsafe_schema_action, nullptr);
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "installing SQLite schema safety policy");
  }

  execute_one(database, "PRAGMA foreign_keys=ON;");
  if (query_integer(database, "PRAGMA foreign_keys;") != 1) {
    throw Error{ErrorCode::unsupported, "SQLite foreign-key enforcement could not be enabled"};
  }
}

[[nodiscard]] int open_flags(const OpenMode mode) {
  constexpr auto common = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_PRIVATECACHE;
  switch (mode) {
  case OpenMode::read_only:
    return SQLITE_OPEN_READONLY | common;
  case OpenMode::read_write:
    return SQLITE_OPEN_READWRITE | common;
  case OpenMode::read_write_create:
    return SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | common;
  }
  return SQLITE_OPEN_READONLY | common;
}

void validate_database_path(const std::filesystem::path& path) {
  const auto locator = path.string();
  if (locator.empty()) {
    throw Error{ErrorCode::database, "SQLite target path must not be empty"};
  }
  if (locator == ":memory:" || locator.starts_with("file:") ||
      locator.find_first_of("?#") != std::string::npos || locator.find('\0') != std::string::npos) {
    throw Error{ErrorCode::database, "SQLite target must be a plain persistent filesystem path"};
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw Error{ErrorCode::database, "cannot inspect SQLite target path: " + error.message()};
  }
  if (!error && std::filesystem::is_symlink(status)) {
    throw Error{ErrorCode::database, "SQLite target must not be a symbolic link"};
  }
  if (!error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
    throw Error{ErrorCode::database, "SQLite target must be a regular file"};
  }
}

void restore_foreign_keys(sqlite3* database, const int enabled) {
  if (sqlite3_get_autocommit(database) == 0) {
    execute_one(database, "ROLLBACK;");
  }
  execute_one(database, enabled != 0 ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;");
}

void execute_cleanup(sqlite3* database, const char* sql) noexcept {
  sqlite3_stmt* statement = nullptr;
  const char* tail = nullptr;
  if (sqlite3_prepare_v3(database, sql, -1, 0, &statement, &tail) == SQLITE_OK &&
      statement != nullptr) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
    }
  }
  if (statement != nullptr) {
    static_cast<void>(sqlite3_finalize(statement));
  }
}

void rollback_noexcept(sqlite3* database) noexcept {
  if (sqlite3_get_autocommit(database) == 0) {
    execute_cleanup(database, "ROLLBACK;");
  }
}

void restore_foreign_keys_noexcept(sqlite3* database, const int enabled) noexcept {
  rollback_noexcept(database);
  execute_cleanup(database, enabled != 0 ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;");
}

void execute_unit_plain(sqlite3* database, const ParsedScript& parsed, const ExecutionUnit& unit) {
  for (const auto& statement : unit.statements) {
    const auto text =
        std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin);
    execute_one(database, text,
                statement.kind == StatementKind::session &&
                    pragma_name(text) == "foreign_key_check");
  }
}

void copy_database(sqlite3* source, sqlite3* destination) {
  DeferredForeignKeysGuard source_deferral{source};
  DeferredForeignKeysGuard destination_deferral{destination};
  auto* backup = sqlite3_backup_init(destination, "main", source, "main");
  if (backup == nullptr) {
    throw_sqlite(destination, sqlite3_errcode(destination), "creating SQLite backup");
  }

  constexpr int pages_per_step = 128;
  constexpr int maximum_busy_retries = 500;
  int busy_retries = 0;
  int step_result = SQLITE_OK;
  while (step_result == SQLITE_OK || step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
    if (step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
      if (++busy_retries > maximum_busy_retries) {
        break;
      }
      static_cast<void>(sqlite3_sleep(10));
    }
    step_result = sqlite3_backup_step(backup, pages_per_step);
  }
  const auto step_error =
      step_result == SQLITE_DONE
          ? std::string{}
          : sqlite_error_message(destination, step_result, "copying SQLite database");
  const auto finish_result = sqlite3_backup_finish(backup);
  if (step_result != SQLITE_DONE) {
    throw Error{ErrorCode::database, step_error};
  }
  if (finish_result != SQLITE_OK) {
    throw_sqlite(destination, finish_result, "finishing SQLite backup");
  }
}

} // namespace dbdiff::sqlite::detail
