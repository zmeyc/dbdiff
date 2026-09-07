#include "connection.hpp"
#include "database_impl.hpp"
#include "dbdiff/hash.hpp"
#include "dbdiff/migration.hpp"
#include "errors.hpp"
#include "schema.hpp"
#include "sql.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbdiff::sqlite::detail {
[[nodiscard]] bool is_lower_sha256(const std::string_view value) {
  return value.size() == 64U &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

[[nodiscard]] std::size_t checked_ordinal(const sqlite3_int64 value) {
  if (value < 0 || static_cast<unsigned long long>(value) >
                       static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
    throw_history_corruption("unit or revision ordinal is out of range");
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] bool history_storage_exists(sqlite3* database) {
  DeferredForeignKeysGuard preserve_deferral{database};
  const auto count = query_integer(
      database, "SELECT count(*) FROM main.sqlite_schema "
                "WHERE lower(name) IN "
                "('_dbdiff_migrations','_dbdiff_migration_revisions','_dbdiff_migration_units');");
  if (count == 0) {
    return false;
  }
  if (count != 3) {
    throw_history_corruption("metadata tables are incomplete");
  }
  return true;
}

template <typename Function> void internal_transaction(sqlite3* database, Function&& function) {
  DeferredForeignKeysGuard preserve_deferral{database};
  if (sqlite3_get_autocommit(database) == 0) {
    throw_history_corruption("an internal transaction was requested inside an active transaction");
  }
  execute_one(database, "BEGIN IMMEDIATE;");
  try {
    function();
    execute_one(database, "COMMIT;");
  } catch (...) {
    rollback_noexcept(database);
    throw;
  }
}

void ensure_history_storage(sqlite3* database) {
  internal_transaction(database, [database] {
    if (history_storage_exists(database)) {
      return;
    }
    execute_one(database, "CREATE TABLE main.\"_dbdiff_migrations\"("
                          "version TEXT PRIMARY KEY,"
                          "backend TEXT NOT NULL CHECK(backend='sqlite'),"
                          "engine_version TEXT NOT NULL,"
                          "attempted_file_sha256 TEXT NOT NULL,"
                          "completed_file_sha256 TEXT"
                          ") STRICT;");
    execute_one(database, "CREATE TABLE main.\"_dbdiff_migration_revisions\"("
                          "version TEXT NOT NULL,"
                          "revision_ordinal INTEGER NOT NULL CHECK(revision_ordinal>=0),"
                          "exact_file_sha256 TEXT NOT NULL,"
                          "exact_sql BLOB NOT NULL,"
                          "PRIMARY KEY(version,revision_ordinal),"
                          "UNIQUE(version,exact_file_sha256),"
                          "FOREIGN KEY(version) REFERENCES \"_dbdiff_migrations\"(version) "
                          "ON DELETE RESTRICT"
                          ") STRICT;");
    execute_one(database, "CREATE TABLE main.\"_dbdiff_migration_units\"("
                          "version TEXT NOT NULL,"
                          "ordinal INTEGER NOT NULL CHECK(ordinal>=0),"
                          "exact_sha256 TEXT NOT NULL,"
                          "explicit_transaction INTEGER NOT NULL "
                          "CHECK(explicit_transaction IN (0,1)),"
                          "before_schema_sha256 TEXT NOT NULL,"
                          "after_schema_sha256 TEXT NOT NULL,"
                          "completed_revision_sha256 TEXT NOT NULL,"
                          "state TEXT NOT NULL CHECK(state IN ('started','completed')),"
                          "PRIMARY KEY(version,ordinal),"
                          "FOREIGN KEY(version) REFERENCES \"_dbdiff_migrations\"(version) "
                          "ON DELETE RESTRICT"
                          ") STRICT;");
  });
}

[[nodiscard]] MigrationHistory read_history_database(sqlite3* database) {
  DeferredForeignKeysGuard preserve_deferral{database};
  if (!history_storage_exists(database)) {
    return {};
  }

  MigrationHistory history;
  history.initialized = true;
  Statement migrations{
      database, "SELECT version,backend,engine_version,attempted_file_sha256,completed_file_sha256 "
                "FROM main.\"_dbdiff_migrations\" ORDER BY version COLLATE BINARY;"};
  while (migrations.step() == SQLITE_ROW) {
    MigrationHistoryEntry entry;
    entry.version = migrations.text(0);
    entry.backend = migrations.text(1);
    entry.engine_version = migrations.text(2);
    entry.attempted_file_sha256 = migrations.text(3);
    entry.completed_file_sha256 = migrations.optional_text(4);
    if (entry.version.empty() || entry.backend != "sqlite" || entry.engine_version.empty() ||
        !is_lower_sha256(entry.attempted_file_sha256) ||
        (entry.completed_file_sha256.has_value() &&
         !is_lower_sha256(*entry.completed_file_sha256))) {
      throw_history_corruption("migration row contains invalid values");
    }
    try {
      validate_engine_version(BackendKind::sqlite, entry.engine_version);
    } catch (const Error&) {
      throw_history_corruption("migration row contains an invalid engine version");
    }

    Statement units{database,
                    "SELECT ordinal,exact_sha256,explicit_transaction,before_schema_sha256,"
                    "after_schema_sha256,state FROM main.\"_dbdiff_migration_units\" "
                    "WHERE version=?1 ORDER BY ordinal;"};
    units.bind_text(1, entry.version);
    bool saw_started = false;
    while (units.step() == SQLITE_ROW) {
      MigrationUnitRecord unit;
      unit.ordinal = checked_ordinal(units.integer64(0));
      unit.exact_sha256 = units.text(1);
      const auto explicit_transaction = units.integer(2);
      unit.explicit_transaction = explicit_transaction != 0;
      unit.before_schema_sha256 = units.text(3);
      unit.after_schema_sha256 = units.text(4);
      const auto state = units.text(5);
      if (unit.ordinal != entry.units.size() ||
          (explicit_transaction != 0 && explicit_transaction != 1) ||
          !is_lower_sha256(unit.exact_sha256) || !is_lower_sha256(unit.before_schema_sha256) ||
          !is_lower_sha256(unit.after_schema_sha256)) {
        throw_history_corruption("migration unit row contains invalid values");
      }
      if (state == "started") {
        if (saw_started) {
          throw_history_corruption("migration contains multiple started units");
        }
        saw_started = true;
        unit.state = MigrationUnitState::started;
      } else if (state == "completed") {
        if (saw_started) {
          throw_history_corruption("a completed unit follows a started unit");
        }
        unit.state = MigrationUnitState::completed;
      } else {
        throw_history_corruption("migration unit has an unknown state");
      }
      entry.units.push_back(std::move(unit));
    }
    if (entry.completed_file_sha256.has_value() && saw_started) {
      throw_history_corruption("a completed migration contains a started unit");
    }
    history.entries.push_back(std::move(entry));
  }

  Statement revisions{database, "SELECT version,revision_ordinal,exact_file_sha256,exact_sql "
                                "FROM main.\"_dbdiff_migration_revisions\" WHERE 0;"};
  if (revisions.step() != SQLITE_DONE) {
    throw_history_corruption("revision metadata validation returned rows");
  }
  return history;
}

void require_history_inputs(const std::string_view version,
                            const std::string_view exact_file_sha256, const std::string_view sql) {
  if (version.empty() || version.size() > 512U || version.find('\0') != std::string_view::npos) {
    throw_migration("migration version must be a non-empty value without NUL bytes");
  }
  if (!is_lower_sha256(exact_file_sha256)) {
    throw_migration("migration file checksum must be a lowercase SHA-256 digest");
  }
  if (sha256_hex(sql) != exact_file_sha256) {
    throw_migration("migration file checksum does not match its exact SQL bytes");
  }
}

void record_attempt(sqlite3* database, const std::string_view version,
                    const std::string_view exact_file_sha256, const std::string_view sql,
                    const bool existing) {
  internal_transaction(database, [&] {
    if (existing) {
      Statement update{database, "UPDATE main.\"_dbdiff_migrations\" "
                                 "SET engine_version=?2,attempted_file_sha256=?3 "
                                 "WHERE version=?1 AND completed_file_sha256 IS NULL;"};
      update.bind_text(1, version);
      update.bind_text(2, sqlite3_libversion());
      update.bind_text(3, exact_file_sha256);
      if (update.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
        throw_history_corruption("could not update the incomplete migration attempt");
      }
    } else {
      Statement insert{database, "INSERT INTO main.\"_dbdiff_migrations\"("
                                 "version,backend,engine_version,attempted_file_sha256) "
                                 "VALUES(?1,'sqlite',?2,?3);"};
      insert.bind_text(1, version);
      insert.bind_text(2, sqlite3_libversion());
      insert.bind_text(3, exact_file_sha256);
      if (insert.step() != SQLITE_DONE) {
        throw_history_corruption("could not insert the migration attempt");
      }
    }

    Statement revision{database,
                       "INSERT INTO main.\"_dbdiff_migration_revisions\"("
                       "version,revision_ordinal,exact_file_sha256,exact_sql) "
                       "SELECT ?1,COALESCE((SELECT max(revision_ordinal)+1 "
                       "FROM main.\"_dbdiff_migration_revisions\" WHERE version=?1),0),?2,?3 "
                       "WHERE NOT EXISTS(SELECT 1 FROM main.\"_dbdiff_migration_revisions\" "
                       "WHERE version=?1 AND exact_file_sha256=?2);"};
    revision.bind_text(1, version);
    revision.bind_text(2, exact_file_sha256);
    revision.bind_blob(3, sql);
    if (revision.step() != SQLITE_DONE) {
      throw_history_corruption("could not store the exact migration revision");
    }
  });
}

void insert_unit_record(sqlite3* database, const std::string_view version,
                        const MigrationUnitRecord& unit,
                        const std::string_view completed_revision_sha256) {
  Statement insert{database,
                   "INSERT INTO main.\"_dbdiff_migration_units\"("
                   "version,ordinal,exact_sha256,explicit_transaction,before_schema_sha256,"
                   "after_schema_sha256,completed_revision_sha256,state) "
                   "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);"};
  insert.bind_text(1, version);
  insert.bind_integer(2, static_cast<sqlite3_int64>(unit.ordinal));
  insert.bind_text(3, unit.exact_sha256);
  insert.bind_integer(4, unit.explicit_transaction ? 1 : 0);
  insert.bind_text(5, unit.before_schema_sha256);
  insert.bind_text(6, unit.after_schema_sha256);
  insert.bind_text(7, completed_revision_sha256);
  insert.bind_text(8, unit.state == MigrationUnitState::completed ? "completed" : "started");
  if (insert.step() != SQLITE_DONE) {
    throw_history_corruption("could not record a migration unit");
  }
}

void insert_unit_record_atomic(sqlite3* database, const std::string_view version,
                               const MigrationUnitRecord& unit,
                               const std::string_view completed_revision_sha256) {
  internal_transaction(
      database, [&] { insert_unit_record(database, version, unit, completed_revision_sha256); });
}

void complete_started_unit(sqlite3* database, const std::string_view version,
                           const std::size_t ordinal) {
  internal_transaction(database, [&] {
    Statement update{database, "UPDATE main.\"_dbdiff_migration_units\" SET state='completed' "
                               "WHERE version=?1 AND ordinal=?2 AND state='started';"};
    update.bind_text(1, version);
    update.bind_integer(2, static_cast<sqlite3_int64>(ordinal));
    if (update.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
      throw_history_corruption("could not complete a started migration unit");
    }
  });
}

void remove_started_unit(sqlite3* database, const std::string_view version,
                         const std::size_t ordinal) {
  internal_transaction(database, [&] {
    Statement remove{database, "DELETE FROM main.\"_dbdiff_migration_units\" "
                               "WHERE version=?1 AND ordinal=?2 AND state='started';"};
    remove.bind_text(1, version);
    remove.bind_integer(2, static_cast<sqlite3_int64>(ordinal));
    if (remove.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
      throw_history_corruption("could not discard a non-applied standalone unit");
    }
  });
}

void complete_migration(sqlite3* database, const std::string_view version,
                        const std::string_view exact_file_sha256) {
  internal_transaction(database, [&] {
    Statement update{database, "UPDATE main.\"_dbdiff_migrations\" SET completed_file_sha256=?2 "
                               "WHERE version=?1 AND completed_file_sha256 IS NULL;"};
    update.bind_text(1, version);
    update.bind_text(2, exact_file_sha256);
    if (update.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
      throw_history_corruption("could not mark the migration complete");
    }
  });
}

[[nodiscard]] bool is_replayable_session_unit(const ExecutionUnit& unit) {
  return !unit.explicit_transaction && unit.statements.size() == 1U &&
         unit.statements.front().kind == StatementKind::session;
}

} // namespace dbdiff::sqlite::detail

namespace dbdiff::sqlite {
using namespace detail;
StartedUnitResolution classify_started_unit(const MigrationUnitRecord& unit,
                                            const std::string_view current_schema_sha256) {
  if (unit.state != MigrationUnitState::started || unit.explicit_transaction) {
    throw_history_corruption("only a started standalone unit can be reconciled");
  }
  if (!is_lower_sha256(unit.before_schema_sha256) || !is_lower_sha256(unit.after_schema_sha256) ||
      !is_lower_sha256(current_schema_sha256)) {
    throw_history_corruption("standalone unit checkpoint contains an invalid schema hash");
  }
  if (unit.before_schema_sha256 == unit.after_schema_sha256) {
    throw_migration("cannot safely reconcile standalone migration unit " +
                    std::to_string(unit.ordinal + 1U) +
                    " because its before and after schemas are indistinguishable");
  }
  if (current_schema_sha256 == unit.after_schema_sha256) {
    return StartedUnitResolution::complete;
  }
  if (current_schema_sha256 == unit.before_schema_sha256) {
    return StartedUnitResolution::retry;
  }
  throw_migration("cannot safely reconcile standalone migration unit " +
                  std::to_string(unit.ordinal + 1U) +
                  "; the schema matches neither its before nor after checkpoint");
}

MigrationHistory Database::read_history() const {
  OperationDeadline deadline{implementation_->progress};
  return read_history_database(implementation_->handle);
}

std::vector<MigrationRevision> Database::recover_revisions(const std::string_view version) const {
  OperationDeadline deadline{implementation_->progress};
  DeferredForeignKeysGuard preserve_deferral{implementation_->handle};
  if (!history_storage_exists(implementation_->handle)) {
    return {};
  }
  Statement revisions{implementation_->handle,
                      "SELECT revision_ordinal,exact_file_sha256,exact_sql "
                      "FROM main.\"_dbdiff_migration_revisions\" "
                      "WHERE version=?1 ORDER BY revision_ordinal;"};
  revisions.bind_text(1, version);
  std::vector<MigrationRevision> result;
  while (revisions.step() == SQLITE_ROW) {
    MigrationRevision revision;
    revision.ordinal = checked_ordinal(revisions.integer64(0));
    revision.exact_file_sha256 = revisions.text(1);
    revision.sql = revisions.blob(2);
    if (revision.ordinal != result.size() || !is_lower_sha256(revision.exact_file_sha256) ||
        sha256_hex(revision.sql) != revision.exact_file_sha256) {
      throw_history_corruption("stored migration revision is corrupt");
    }
    result.push_back(std::move(revision));
  }
  return result;
}

MigrationApplyResult Database::apply_version(const std::string_view version,
                                             const std::string_view exact_file_sha256,
                                             const std::string_view sql, const bool resume) {
  OperationDeadline deadline{implementation_->progress};
  require_history_inputs(version, exact_file_sha256, sql);
  auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  auto parsed = build_execution_units(std::string{sql}, std::move(statements));
  if (sqlite3_get_autocommit(implementation_->handle) == 0) {
    throw_migration("cannot apply a SQLite migration inside an active transaction");
  }

  ensure_history_storage(implementation_->handle);
  auto history = read_history_database(implementation_->handle);
  auto found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
  const auto existing = found != history.entries.end();
  if (existing) {
    const auto completed_file_sha256 = found->completed_file_sha256;
    if (completed_file_sha256.has_value()) {
      if (found->attempted_file_sha256 != *completed_file_sha256) {
        throw_history_corruption("completed migration checksum does not match its last attempt");
      }
      if (*completed_file_sha256 != exact_file_sha256) {
        throw_migration("completed migration '" + std::string{version} +
                        "' has different exact SQL bytes");
      }
      return MigrationApplyResult{true, found->units.size(), *completed_file_sha256};
    }
  }
  if (existing && !resume) {
    throw_migration("migration '" + std::string{version} +
                    "' is incomplete; explicit resume is required");
  }
  if (!existing && resume) {
    throw_migration("migration '" + std::string{version} + "' has no incomplete attempt to resume");
  }
  if (existing && found->engine_version != sqlite3_libversion()) {
    throw Error{ErrorCode::unsupported,
                "cannot resume a SQLite migration attempt with a different engine version"};
  }

  record_attempt(implementation_->handle, version, exact_file_sha256, parsed.sql, existing);
  history = read_history_database(implementation_->handle);
  found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
  if (found == history.entries.end() || found->completed_file_sha256.has_value()) {
    throw_history_corruption("attempted migration row could not be read back");
  }

  if (!found->units.empty() && found->units.back().state == MigrationUnitState::started) {
    const auto started = found->units.back();
    const auto resolution =
        classify_started_unit(started, inspect_database(implementation_->handle).semantic_hash);
    if (resolution == StartedUnitResolution::complete) {
      if (started.ordinal >= parsed.units.size() ||
          parsed.units[started.ordinal].exact_sha256 != started.exact_sha256 ||
          parsed.units[started.ordinal].explicit_transaction != started.explicit_transaction) {
        throw_migration("cannot safely resume an edited standalone migration unit that may "
                        "already be applied");
      }
      complete_started_unit(implementation_->handle, version, started.ordinal);
    } else {
      remove_started_unit(implementation_->handle, version, started.ordinal);
    }
    history = read_history_database(implementation_->handle);
    found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
    if (found == history.entries.end()) {
      throw_history_corruption("migration disappeared while reconciling its last unit");
    }
  }

  std::vector<std::string> completed_hashes;
  completed_hashes.reserve(found->units.size());
  for (const auto& unit : found->units) {
    if (unit.state != MigrationUnitState::completed) {
      throw_history_corruption("a started unit remained after reconciliation");
    }
    completed_hashes.push_back(unit.exact_sha256);
  }
  validate_completed_prefix(completed_hashes, parsed);
  for (std::size_t index = 0; index < found->units.size(); ++index) {
    if (found->units[index].explicit_transaction != parsed.units[index].explicit_transaction) {
      throw_history_corruption("stored unit transaction shape differs from its exact SQL");
    }
  }

  const auto completed_unit_count = found->units.size();
  const auto foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");
  try {
    std::optional<std::size_t> last_completed_sql;
    for (std::size_t index = 0; index < completed_unit_count; ++index) {
      if (!is_replayable_session_unit(parsed.units[index])) {
        last_completed_sql = index;
      }
    }
    for (std::size_t index = 0; index < completed_unit_count; ++index) {
      if (is_replayable_session_unit(parsed.units[index])) {
        const auto& statement = parsed.units[index].statements.front();
        const auto text =
            std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin);
        if (pragma_name(text) == "foreign_key_check") {
          // A later SQL commit supersedes this check's session effects and
          // may have removed the table it checked. Otherwise consume its rows
          // without revalidating later application data: whether SQLite opens
          // a read transaction determines whether deferral is reset.
          if (!last_completed_sql.has_value() || index > *last_completed_sql) {
            execute_one(implementation_->handle, text);
          }
        } else {
          execute_unit_plain(implementation_->handle, parsed, parsed.units[index]);
        }
      } else {
        // A completed SQL transaction consumed any preceding deferral directive.
        execute_one(implementation_->handle, "PRAGMA defer_foreign_keys=OFF;");
      }
    }

    for (std::size_t index = completed_unit_count; index < parsed.units.size(); ++index) {
      const auto& execution_unit = parsed.units[index];
      const auto before_schema_sha256 = inspect_database(implementation_->handle).semantic_hash;

      if (is_replayable_session_unit(execution_unit)) {
        execute_unit_plain(implementation_->handle, parsed, execution_unit);
        const auto after_schema_sha256 = inspect_database(implementation_->handle).semantic_hash;
        if (before_schema_sha256 != after_schema_sha256) {
          throw_migration("SQLite session directive unexpectedly changed the persistent schema");
        }
        insert_unit_record_atomic(implementation_->handle, version,
                                  MigrationUnitRecord{index, execution_unit.exact_sha256, false,
                                                      before_schema_sha256, after_schema_sha256,
                                                      MigrationUnitState::completed},
                                  exact_file_sha256);
        continue;
      }

      if (execution_unit.explicit_transaction) {
        bool checkpointed = false;
        for (const auto& statement : execution_unit.statements) {
          const auto text =
              std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin);
          if (statement.kind == StatementKind::commit) {
            const auto after_schema_sha256 =
                inspect_database(implementation_->handle).semantic_hash;
            insert_unit_record(implementation_->handle, version,
                               MigrationUnitRecord{index, execution_unit.exact_sha256, true,
                                                   before_schema_sha256, after_schema_sha256,
                                                   MigrationUnitState::completed},
                               exact_file_sha256);
            execute_one(implementation_->handle, text);
            checkpointed = true;
          } else {
            execute_one(implementation_->handle, text,
                        statement.kind == StatementKind::session &&
                            pragma_name(text) == "foreign_key_check");
          }
        }
        if (!checkpointed || sqlite3_get_autocommit(implementation_->handle) == 0) {
          throw_migration("SQLite explicit transaction unit did not commit cleanly");
        }
        continue;
      }

      auto clone = Database::temporary(implementation_->settings);
      copy_database(implementation_->handle, clone.implementation_->handle);
      const auto unit_foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");
      execute_one(clone.implementation_->handle,
                  unit_foreign_keys != 0 ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;");
      const auto unit_deferred =
          query_integer(implementation_->handle, "PRAGMA defer_foreign_keys;");
      execute_one(clone.implementation_->handle, unit_deferred != 0
                                                     ? "PRAGMA defer_foreign_keys=ON;"
                                                     : "PRAGMA defer_foreign_keys=OFF;");
      execute_unit_plain(clone.implementation_->handle, parsed, execution_unit);
      const auto expected_after_schema_sha256 = clone.inspect().semantic_hash;
      insert_unit_record_atomic(
          implementation_->handle, version,
          MigrationUnitRecord{index, execution_unit.exact_sha256, false, before_schema_sha256,
                              expected_after_schema_sha256, MigrationUnitState::started},
          exact_file_sha256);
      execute_unit_plain(implementation_->handle, parsed, execution_unit);
      const auto actual_after_schema_sha256 =
          inspect_database(implementation_->handle).semantic_hash;
      if (actual_after_schema_sha256 != expected_after_schema_sha256) {
        throw_migration("standalone SQLite DDL produced a schema different from its preflight");
      }
      complete_started_unit(implementation_->handle, version, index);
    }

    restore_foreign_keys(implementation_->handle, foreign_keys);
    static_cast<void>(inspect_database(implementation_->handle));
    complete_migration(implementation_->handle, version, exact_file_sha256);
  } catch (...) {
    restore_foreign_keys_noexcept(implementation_->handle, foreign_keys);
    throw;
  }
  return MigrationApplyResult{false, parsed.units.size(), std::string{exact_file_sha256}};
}

} // namespace dbdiff::sqlite
