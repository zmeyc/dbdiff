#include "database_impl.hpp"
#include "dbdiff/sqlite.hpp"
#include "errors.hpp"
#include "schema.hpp"
#include "sql.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dbdiff::sqlite {
using namespace detail;
Database::Database(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

Database::~Database() = default;
Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Database Database::temporary(const ConnectionSettings settings) {
  return Database{std::make_unique<Impl>(std::string{}, OpenMode::read_write_create, settings)};
}

Database Database::open(const std::filesystem::path& path, const OpenMode mode,
                        const ConnectionSettings settings) {
  validate_database_path(path);
  return Database{std::make_unique<Impl>(path.string(), mode, settings)};
}

void Database::execute_source(const std::string_view sql) {
  OperationDeadline deadline{implementation_->progress};
  const auto statements = scan_statements(sql);
  validate_source_statements(sql, statements);

  execute_one(implementation_->handle, "BEGIN IMMEDIATE;");
  try {
    for (const auto& statement : statements) {
      execute_one(implementation_->handle,
                  sql.substr(statement.begin, statement.end - statement.begin));
    }
    static_cast<void>(inspect_database(implementation_->handle));
    execute_one(implementation_->handle, "PRAGMA foreign_key_check;", true);
    execute_one(implementation_->handle, "COMMIT;");
  } catch (...) {
    rollback_noexcept(implementation_->handle);
    throw;
  }
}

void Database::execute_migration(const std::string_view sql) {
  OperationDeadline deadline{implementation_->progress};
  const auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  const auto foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");

  try {
    for (const auto& statement : statements) {
      const auto text = sql.substr(statement.begin, statement.end - statement.begin);
      execute_one(implementation_->handle, text,
                  statement.kind == StatementKind::session &&
                      pragma_name(text) == "foreign_key_check");
    }
    if (sqlite3_get_autocommit(implementation_->handle) == 0) {
      throw Error{ErrorCode::migration,
                  "SQLite migration left the connection inside a transaction"};
    }
    restore_foreign_keys(implementation_->handle, foreign_keys);
    static_cast<void>(inspect_database(implementation_->handle));
  } catch (...) {
    restore_foreign_keys_noexcept(implementation_->handle, foreign_keys);
    throw;
  }
}

void Database::execute_prefix(const std::string_view sql, const std::size_t completed_unit_count) {
  OperationDeadline deadline{implementation_->progress};
  auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  auto parsed = build_execution_units(std::string{sql}, std::move(statements));
  if (completed_unit_count > parsed.units.size()) {
    throw_migration("SQLite migration prefix contains more units than the script");
  }
  const auto foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");
  try {
    for (std::size_t index = 0; index < completed_unit_count; ++index) {
      execute_unit_plain(implementation_->handle, parsed, parsed.units[index]);
    }
    if (sqlite3_get_autocommit(implementation_->handle) == 0) {
      throw_migration("SQLite migration prefix left the connection inside a transaction");
    }
    restore_foreign_keys(implementation_->handle, foreign_keys);
    static_cast<void>(inspect_database(implementation_->handle));
  } catch (...) {
    restore_foreign_keys_noexcept(implementation_->handle, foreign_keys);
    throw;
  }
}

void Database::backup_to(Database& destination) const {
  OperationDeadline source_deadline{implementation_->progress};
  OperationDeadline destination_deadline{destination.implementation_->progress};
  if (implementation_.get() == destination.implementation_.get()) {
    throw Error{ErrorCode::database, "SQLite backup source and destination must be different"};
  }
  if (sqlite3_get_autocommit(implementation_->handle) == 0 ||
      sqlite3_get_autocommit(destination.implementation_->handle) == 0) {
    throw Error{ErrorCode::database,
                "SQLite online backup requires idle source and destination connections"};
  }
  static_cast<void>(inspect_database(implementation_->handle));
  copy_database(implementation_->handle, destination.implementation_->handle);
  static_cast<void>(inspect_database(destination.implementation_->handle));
}

BackendKind kind() noexcept { return BackendKind::sqlite; }

} // namespace dbdiff::sqlite
