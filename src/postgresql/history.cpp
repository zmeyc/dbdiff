#include "internal.hpp"

namespace dbdiff::postgresql {
namespace detail {

[[nodiscard]] bool lower_sha256(const std::string_view value) noexcept {
  return value.size() == 64U &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

void require_file_sha256(const std::string_view value) {
  if (!lower_sha256(value)) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration file hash must be a lowercase SHA-256 digest"};
  }
}

void require_version(const std::string_view version) {
  if (version.empty() || version.size() > 512U || contains_nul(version)) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration version must be 1-512 bytes without NUL"};
  }
}

void ensure_history_tables(pqxx::transaction_base& transaction) {
  execute_no_rows(transaction, "CREATE SCHEMA IF NOT EXISTS \"_dbdiff\"");
  execute_no_rows(
      transaction,
      "CREATE TABLE IF NOT EXISTS \"_dbdiff\".\"migrations\" ("
      "version text PRIMARY KEY,"
      "backend text NOT NULL CHECK (backend = 'postgresql'),"
      "engine_version text NOT NULL,"
      "attempted_file_sha256 text NOT NULL "
      "  CHECK (attempted_file_sha256 ~ '^[0-9a-f]{64}$'),"
      "completed_file_sha256 text "
      "  CHECK (completed_file_sha256 IS NULL OR "
      "         completed_file_sha256 ~ '^[0-9a-f]{64}$'),"
      "created_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp(),"
      "updated_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp()"
      ")");
  execute_no_rows(
      transaction,
      "CREATE TABLE IF NOT EXISTS \"_dbdiff\".\"revisions\" ("
      "version text NOT NULL REFERENCES \"_dbdiff\".\"migrations\"(version) "
      "  ON UPDATE RESTRICT ON DELETE RESTRICT,"
      "ordinal bigint NOT NULL CHECK (ordinal >= 0),"
      "exact_file_sha256 text NOT NULL CHECK (exact_file_sha256 ~ '^[0-9a-f]{64}$'),"
      "sql text NOT NULL,"
      "created_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp(),"
      "PRIMARY KEY (version, ordinal),"
      "UNIQUE (version, exact_file_sha256)"
      ")");
  execute_no_rows(
      transaction,
      "CREATE TABLE IF NOT EXISTS \"_dbdiff\".\"units\" ("
      "version text NOT NULL REFERENCES \"_dbdiff\".\"migrations\"(version) "
      "  ON UPDATE RESTRICT ON DELETE RESTRICT,"
      "ordinal bigint NOT NULL CHECK (ordinal >= 0),"
      "exact_sha256 text NOT NULL CHECK (exact_sha256 ~ '^[0-9a-f]{64}$'),"
      "explicit_transaction boolean NOT NULL,"
      "before_schema_sha256 text NOT NULL "
      "  CHECK (before_schema_sha256 = '' OR before_schema_sha256 ~ '^[0-9a-f]{64}$'),"
      "after_schema_sha256 text NOT NULL "
      "  CHECK (after_schema_sha256 = '' OR after_schema_sha256 ~ '^[0-9a-f]{64}$'),"
      "state text NOT NULL CHECK (state IN ('started', 'completed')),"
      "revision_ordinal bigint NOT NULL CHECK (revision_ordinal >= 0),"
      "updated_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp(),"
      "PRIMARY KEY (version, ordinal),"
      "FOREIGN KEY (version, revision_ordinal) "
      "  REFERENCES \"_dbdiff\".\"revisions\"(version, ordinal) "
      "  ON UPDATE RESTRICT ON DELETE RESTRICT"
      ")");
}

[[nodiscard]] bool history_tables_initialized(pqxx::transaction_base& transaction) {
  const auto row =
      execute_one_row(transaction, "SELECT pg_catalog.to_regclass('_dbdiff.migrations')::text,"
                                   "       pg_catalog.to_regclass('_dbdiff.revisions')::text,"
                                   "       pg_catalog.to_regclass('_dbdiff.units')::text");
  const std::array present{!row[0].is_null(), !row[1].is_null(), !row[2].is_null()};
  const auto count = static_cast<std::size_t>(std::ranges::count(present, true));
  if (count != 0U && count != present.size()) {
    throw Error{ErrorCode::drift, "PostgreSQL migration history tables are incomplete"};
  }
  return count == present.size();
}

template <typename Field>
[[nodiscard]] std::size_t history_ordinal(const Field& field, const std::string_view description) {
  const auto value = field.template as<std::uint64_t>();
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw Error{ErrorCode::drift, std::string{description} + " exceeds this build's size limit"};
  }
  return static_cast<std::size_t>(value);
}

void validate_stored_hash(const std::string_view hash, const std::string_view description) {
  if (!lower_sha256(hash)) {
    throw Error{ErrorCode::drift,
                "PostgreSQL migration history contains an invalid " + std::string{description}};
  }
}

[[nodiscard]] MigrationHistory read_history_unlocked(pqxx::transaction_base& transaction) {
  MigrationHistory history;
  history.initialized = history_tables_initialized(transaction);
  if (!history.initialized) {
    return history;
  }

  const auto migration_rows = transaction.exec(
      "SELECT version, backend, engine_version, attempted_file_sha256, completed_file_sha256 "
      "FROM \"_dbdiff\".\"migrations\" ORDER BY version COLLATE \"C\"");
  std::map<std::string, std::size_t, std::less<>> entry_indexes;
  history.entries.reserve(static_cast<std::size_t>(migration_rows.size()));
  for (const auto& row : migration_rows) {
    auto attempted_hash = row[3].as<std::string>();
    validate_stored_hash(attempted_hash, "attempted file hash");
    std::optional<std::string> completed_hash;
    if (!row[4].is_null()) {
      completed_hash = row[4].as<std::string>();
      validate_stored_hash(*completed_hash, "completed file hash");
    }
    auto version = row[0].as<std::string>();
    const auto backend = row[1].as<std::string>();
    const auto engine_version = row[2].as<std::string>();
    if (version.empty() || contains_nul(version) || backend != "postgresql" ||
        engine_version.empty() ||
        (completed_hash.has_value() && *completed_hash != attempted_hash)) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history contains an invalid migration record"};
    }
    try {
      validate_engine_version(BackendKind::postgresql, engine_version);
    } catch (const Error&) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history contains an invalid engine version"};
    }
    const auto index = history.entries.size();
    if (!entry_indexes.emplace(version, index).second) {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains a duplicate version"};
    }
    history.entries.push_back(MigrationHistoryEntry{
        .version = std::move(version),
        .backend = backend,
        .engine_version = engine_version,
        .attempted_file_sha256 = std::move(attempted_hash),
        .completed_file_sha256 = std::move(completed_hash),
        .units = {},
    });
  }

  const auto unit_rows =
      transaction.exec("SELECT version, ordinal, exact_sha256, explicit_transaction, "
                       "       before_schema_sha256, after_schema_sha256, state "
                       "FROM \"_dbdiff\".\"units\" ORDER BY version COLLATE \"C\", ordinal");
  for (const auto& row : unit_rows) {
    const auto version = row[0].as<std::string>();
    const auto entry = entry_indexes.find(version);
    if (entry == entry_indexes.end()) {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains an orphan unit"};
    }
    auto exact_hash = row[2].as<std::string>();
    validate_stored_hash(exact_hash, "unit hash");
    const auto state_text = row[6].as<std::string>();
    MigrationUnitState state{};
    if (state_text == "started") {
      state = MigrationUnitState::started;
    } else if (state_text == "completed") {
      state = MigrationUnitState::completed;
    } else {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains an invalid unit state"};
    }
    auto& units = history.entries[entry->second].units;
    const auto ordinal = history_ordinal(row[1], "migration unit ordinal");
    const auto before_hash = row[4].as<std::string>();
    const auto after_hash = row[5].as<std::string>();
    if (ordinal != units.size() || !lower_sha256(before_hash) ||
        (state == MigrationUnitState::completed && !lower_sha256(after_hash)) ||
        (state == MigrationUnitState::started &&
         (row[3].as<bool>() || !after_hash.empty() ||
          std::ranges::any_of(units, [](const MigrationUnitRecord& unit) {
            return unit.state == MigrationUnitState::started;
          })))) {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains an invalid unit record"};
    }
    if (state == MigrationUnitState::completed &&
        std::ranges::any_of(units, [](const MigrationUnitRecord& unit) {
          return unit.state == MigrationUnitState::started;
        })) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history has a completed unit after a started unit"};
    }
    units.push_back(MigrationUnitRecord{
        .ordinal = ordinal,
        .exact_sha256 = std::move(exact_hash),
        .explicit_transaction = row[3].as<bool>(),
        .before_schema_sha256 = before_hash,
        .after_schema_sha256 = after_hash,
        .state = state,
    });
  }
  for (const auto& entry : history.entries) {
    if (entry.completed_file_sha256.has_value() &&
        std::ranges::any_of(entry.units, [](const MigrationUnitRecord& unit) {
          return unit.state == MigrationUnitState::started;
        })) {
      throw Error{ErrorCode::drift, "completed PostgreSQL migration contains a started unit"};
    }
  }
  return history;
}

void hash_result(Sha256& hash, const std::string_view name, const pqxx::result& rows) {
  hash.add_length_prefixed(name);
  hash.add_length_prefixed(std::to_string(rows.size()));
  for (const auto& row : rows) {
    hash.add_length_prefixed("row");
    for (const auto& field : row) {
      hash.add_length_prefixed(field.is_null() ? "null" : "value");
      if (!field.is_null()) {
        hash.add_length_prefixed(field.as<std::string>());
      }
    }
  }
}

[[nodiscard]] std::string catalog_fingerprint(pqxx::transaction_base& transaction) {
  constexpr std::string_view schema_predicate =
      "n.nspname <> '_dbdiff' AND n.nspname <> 'information_schema' "
      "AND n.nspname !~ '^pg_'";
  Sha256 hash;
  hash.add_length_prefixed("dbdiff.postgresql.catalog-checkpoint.v2");
  hash_result(hash, "schemas",
              transaction.exec("SELECT n.nspname FROM pg_catalog.pg_namespace AS n WHERE " +
                               std::string{schema_predicate} +
                               " ORDER BY n.nspname COLLATE \"C\""));
  hash_result(
      hash, "relations",
      transaction.exec("SELECT n.nspname, c.relname, c.relkind::text, c.relpersistence::text, "
                       "       c.relrowsecurity, c.relforcerowsecurity, c.reloptions::text "
                       "FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " AND c.relkind IN ('r','p','v','m','S','f','i','I') "
                       "ORDER BY n.nspname COLLATE \"C\", c.relkind, c.relname COLLATE \"C\""));
  hash_result(hash, "columns",
              transaction.exec(
                  "SELECT n.nspname, c.relname, a.attnum, a.attname, "
                  "       a.atttypid::bigint, a.atttypmod, a.attnotnull, ad.adbin::text, "
                  "       a.attidentity::text, a.attgenerated::text "
                  "FROM pg_catalog.pg_attribute AS a "
                  "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                  "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                  "LEFT JOIN pg_catalog.pg_attrdef AS ad "
                  "  ON ad.adrelid = a.attrelid AND ad.adnum = a.attnum "
                  "WHERE " +
                  std::string{schema_predicate} +
                  " AND c.relkind IN ('r','p','v','m','f') AND a.attnum > 0 AND NOT a.attisdropped "
                  "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", a.attnum"));
  hash_result(
      hash, "constraints",
      transaction.exec("SELECT n.nspname, c.relname, con.conname, con.contype::text, "
                       "       pg_catalog.pg_get_constraintdef(con.oid, false), con.convalidated "
                       "FROM pg_catalog.pg_constraint AS con "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "          con.conname COLLATE \"C\""));
  hash_result(
      hash, "indexes",
      transaction.exec("SELECT n.nspname, c.relname, pg_catalog.pg_get_indexdef(c.oid, 0, false) "
                       "FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " AND c.relkind IN ('i', 'I') "
                       "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\""));
  hash_result(
      hash, "policies",
      transaction.exec("SELECT n.nspname, c.relname, p.polname, p.polcmd::text, p.polpermissive, "
                       "       p.polroles::text, p.polqual::text, p.polwithcheck::text "
                       "FROM pg_catalog.pg_policy AS p "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "          p.polname COLLATE \"C\""));
  return hash.finish_hex();
}

[[nodiscard]] std::optional<MigrationHistoryEntry> history_entry(const MigrationHistory& history,
                                                                 const std::string_view version) {
  const auto found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
  if (found == history.entries.end()) {
    return std::nullopt;
  }
  return *found;
}

void validate_history_for_apply(const MigrationHistory& history, const std::string_view version,
                                const bool version_exists) {
  for (const auto& entry : history.entries) {
    if (entry.backend != "postgresql") {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history contains a record for another backend"};
    }
    if (!entry.completed_file_sha256.has_value() && entry.version != version) {
      throw Error{ErrorCode::migration,
                  "another PostgreSQL migration version has an incomplete attempt: " +
                      entry.version};
    }
    if (!version_exists && entry.version > version) {
      throw Error{ErrorCode::migration,
                  "PostgreSQL migration versions must be applied in lexical order"};
    }
  }
}

[[nodiscard]] std::size_t record_revision(pqxx::transaction_base& transaction,
                                          const std::string_view version,
                                          const std::string_view engine_version,
                                          const std::string_view exact_file_sha256,
                                          const std::string_view sql) {
  const auto rows =
      execute_parameters(transaction,
                         "WITH migration AS ("
                         "  INSERT INTO \"_dbdiff\".\"migrations\" "
                         "    (version, backend, engine_version, attempted_file_sha256) "
                         "  VALUES ($1, 'postgresql', $2, $3) "
                         "  ON CONFLICT (version) DO UPDATE "
                         "  SET engine_version = EXCLUDED.engine_version, "
                         "      attempted_file_sha256 = EXCLUDED.attempted_file_sha256, "
                         "      updated_at = pg_catalog.clock_timestamp() "
                         "  WHERE \"_dbdiff\".\"migrations\".completed_file_sha256 IS NULL "
                         "  RETURNING version"
                         "), existing AS ("
                         "  SELECT ordinal FROM \"_dbdiff\".\"revisions\" "
                         "  WHERE version = $1 AND exact_file_sha256 = $3 "
                         "    AND EXISTS (SELECT 1 FROM migration)"
                         "), next_ordinal AS ("
                         "  SELECT COALESCE(MAX(ordinal) + 1, 0)::bigint AS ordinal "
                         "  FROM \"_dbdiff\".\"revisions\" WHERE version = $1"
                         "), inserted AS ("
                         "  INSERT INTO \"_dbdiff\".\"revisions\" "
                         "    (version, ordinal, exact_file_sha256, sql) "
                         "  SELECT $1, next_ordinal.ordinal, $3, $4 FROM next_ordinal "
                         "  WHERE NOT EXISTS (SELECT 1 FROM existing) "
                         "    AND EXISTS (SELECT 1 FROM migration) RETURNING ordinal"
                         ") SELECT ordinal FROM existing UNION ALL SELECT ordinal FROM inserted",
                         version, engine_version, exact_file_sha256, sql);
  if (rows.size() != 1U) {
    throw Error{ErrorCode::database,
                "PostgreSQL could not atomically record the exact migration revision"};
  }
  return history_ordinal(rows.front()[0], "migration revision ordinal");
}

void checkpoint_completed_unit(pqxx::transaction_base& transaction, const std::string_view version,
                               const ExecutionUnit& unit,
                               const std::string_view before_schema_sha256,
                               const std::string_view after_schema_sha256,
                               const std::size_t revision_ordinal, const bool final_unit,
                               const std::string_view exact_file_sha256) {
  if (final_unit) {
    const auto rows = execute_parameters(
        transaction,
        "WITH inserted AS ("
        "  INSERT INTO \"_dbdiff\".\"units\" "
        "    (version, ordinal, exact_sha256, explicit_transaction, "
        "     before_schema_sha256, after_schema_sha256, state, revision_ordinal) "
        "  VALUES ($1, $2, $3, $4, $5, $6, 'completed', $7) RETURNING 1"
        ") "
        "UPDATE \"_dbdiff\".\"migrations\" "
        "SET completed_file_sha256 = $8, attempted_file_sha256 = $8, "
        "    updated_at = pg_catalog.clock_timestamp() "
        "WHERE version = $1 AND EXISTS (SELECT 1 FROM inserted) RETURNING version",
        version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256,
        unit.explicit_transaction, before_schema_sha256, after_schema_sha256,
        static_cast<std::uint64_t>(revision_ordinal), exact_file_sha256);
    if (rows.size() != 1U) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL could not atomically checkpoint the final migration unit"};
    }
    return;
  }
  static_cast<void>(
      execute_parameters(transaction,
                         "INSERT INTO \"_dbdiff\".\"units\" "
                         "  (version, ordinal, exact_sha256, explicit_transaction, "
                         "   before_schema_sha256, after_schema_sha256, state, revision_ordinal) "
                         "VALUES ($1, $2, $3, $4, $5, $6, 'completed', $7)",
                         version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256,
                         unit.explicit_transaction, before_schema_sha256, after_schema_sha256,
                         static_cast<std::uint64_t>(revision_ordinal)));
}

void checkpoint_started_unit(pqxx::transaction_base& transaction, const std::string_view version,
                             const ExecutionUnit& unit, const std::string_view before_schema_sha256,
                             const std::size_t revision_ordinal) {
  static_cast<void>(
      execute_parameters(transaction,
                         "INSERT INTO \"_dbdiff\".\"units\" "
                         "  (version, ordinal, exact_sha256, explicit_transaction, "
                         "   before_schema_sha256, after_schema_sha256, state, revision_ordinal) "
                         "VALUES ($1, $2, $3, false, $4, '', 'started', $5)",
                         version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256,
                         before_schema_sha256, static_cast<std::uint64_t>(revision_ordinal)));
}

void remove_failed_started_unit(pqxx::transaction_base& transaction, const std::string_view version,
                                const ExecutionUnit& unit) {
  static_cast<void>(execute_parameters(
      transaction,
      "DELETE FROM \"_dbdiff\".\"units\" "
      "WHERE version = $1 AND ordinal = $2 AND exact_sha256 = $3 AND state = 'started'",
      version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256));
}

void complete_started_unit(pqxx::transaction_base& transaction, const std::string_view version,
                           const ExecutionUnit& unit, const std::string_view after_schema_sha256,
                           const bool final_unit, const std::string_view exact_file_sha256) {
  if (final_unit) {
    const auto rows = execute_parameters(
        transaction,
        "WITH completed AS ("
        "  UPDATE \"_dbdiff\".\"units\" "
        "  SET state = 'completed', after_schema_sha256 = $4, "
        "      updated_at = pg_catalog.clock_timestamp() "
        "  WHERE version = $1 AND ordinal = $2 AND exact_sha256 = $3 "
        "    AND state = 'started' RETURNING 1"
        ") "
        "UPDATE \"_dbdiff\".\"migrations\" "
        "SET completed_file_sha256 = $5, attempted_file_sha256 = $5, "
        "    updated_at = pg_catalog.clock_timestamp() "
        "WHERE version = $1 AND EXISTS (SELECT 1 FROM completed) RETURNING version",
        version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256, after_schema_sha256,
        exact_file_sha256);
    if (rows.size() != 1U) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL could not atomically checkpoint the final standalone unit"};
    }
    return;
  }
  const auto rows = execute_parameters(
      transaction,
      "UPDATE \"_dbdiff\".\"units\" "
      "SET state = 'completed', after_schema_sha256 = $4, "
      "    updated_at = pg_catalog.clock_timestamp() "
      "WHERE version = $1 AND ordinal = $2 AND exact_sha256 = $3 AND state = 'started' "
      "RETURNING ordinal",
      version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256, after_schema_sha256);
  if (rows.size() != 1U) {
    throw Error{ErrorCode::drift,
                "PostgreSQL could not checkpoint a completed standalone migration unit"};
  }
}

void complete_empty_migration(pqxx::transaction_base& transaction, const std::string_view version,
                              const std::string_view exact_file_sha256) {
  const auto rows =
      execute_parameters(transaction,
                         "UPDATE \"_dbdiff\".\"migrations\" "
                         "SET completed_file_sha256 = $2, attempted_file_sha256 = $2, "
                         "    updated_at = pg_catalog.clock_timestamp() "
                         "WHERE version = $1 AND completed_file_sha256 IS NULL RETURNING version",
                         version, exact_file_sha256);
  if (rows.size() != 1U) {
    throw Error{ErrorCode::drift, "PostgreSQL could not complete an empty migration"};
  }
}

} // namespace detail

using namespace detail;

MigrationApplyResult validate_migration_resume(const std::optional<MigrationHistoryEntry>& history,
                                               const ParsedScript& candidate,
                                               const std::string_view exact_file_sha256,
                                               const bool resume) {
  require_file_sha256(exact_file_sha256);
  if (sha256_hex(candidate.sql) != exact_file_sha256) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration SQL does not match its exact file hash"};
  }
  if (!history.has_value()) {
    if (resume) {
      throw Error{ErrorCode::migration,
                  "cannot resume a PostgreSQL migration version with no stored attempt"};
    }
    return {};
  }

  std::vector<std::string> completed_hashes;
  completed_hashes.reserve(history->units.size());
  for (std::size_t index = 0U; index < history->units.size(); ++index) {
    const auto& unit = history->units[index];
    if (unit.ordinal != index) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history units are not a contiguous prefix"};
    }
    validate_stored_hash(unit.exact_sha256, "unit hash");
    if (!unit.before_schema_sha256.empty()) {
      validate_stored_hash(unit.before_schema_sha256, "unit before-schema hash");
    }
    if (!unit.after_schema_sha256.empty()) {
      validate_stored_hash(unit.after_schema_sha256, "unit after-schema hash");
    }
    if (unit.state == MigrationUnitState::started) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration has an uncertain standalone unit at ordinal " +
                      std::to_string(unit.ordinal) +
                      "; recover the stored revision and reconcile it manually"};
    }
    if (index >= candidate.units.size() ||
        unit.explicit_transaction != candidate.units[index].explicit_transaction) {
      throw Error{ErrorCode::migration,
                  "edited PostgreSQL migration changes a completed unit boundary"};
    }
    completed_hashes.push_back(unit.exact_sha256);
  }
  validate_completed_prefix(completed_hashes, candidate);

  if (history->completed_file_sha256.has_value()) {
    if (*history->completed_file_sha256 != exact_file_sha256) {
      throw Error{ErrorCode::drift,
                  "completed PostgreSQL migration file bytes no longer match history"};
    }
    if (completed_hashes.size() != candidate.units.size()) {
      throw Error{ErrorCode::drift,
                  "completed PostgreSQL migration history has an incomplete unit prefix"};
    }
    return MigrationApplyResult{
        .already_completed = true,
        .completed_unit_count = completed_hashes.size(),
        .completed_file_sha256 = std::string{exact_file_sha256},
    };
  }
  if (!resume) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration has an incomplete attempt; retry with resume enabled"};
  }
  if (!candidate.units.empty() && completed_hashes.size() == candidate.units.size()) {
    throw Error{ErrorCode::drift,
                "PostgreSQL migration completed every unit without an atomic file checkpoint"};
  }
  return MigrationApplyResult{
      .already_completed = false,
      .completed_unit_count = completed_hashes.size(),
      .completed_file_sha256 = {},
  };
}

MigrationHistory Database::read_history() const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    return read_history_unlocked(transaction);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::database, "PostgreSQL migration history read failed"};
  }
}

MigrationApplyResult Database::apply_version(const std::string_view version,
                                             const std::string_view exact_file_sha256,
                                             const std::string_view sql, const bool resume) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  require_version(version);
  require_file_sha256(exact_file_sha256);
  if (!is_valid_utf8(sql)) {
    throw Error{ErrorCode::migration, "PostgreSQL migration SQL must be valid UTF-8"};
  }
  const auto parsed = parse_migration(std::string{sql});
  validate_resumable_session_units(parsed);

  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    ensure_history_tables(transaction);
    const auto history = read_history_unlocked(transaction);
    const auto existing = history_entry(history, version);
    validate_history_for_apply(history, version, existing.has_value());
    auto result = validate_migration_resume(existing, parsed, exact_file_sha256, resume);
    if (result.already_completed) {
      return result;
    }
    if (existing.has_value() &&
        parse_server_version(existing->engine_version).major != implementation_->version.major) {
      throw Error{ErrorCode::unsupported,
                  "cannot resume a PostgreSQL migration attempt from another server major"};
    }

    const auto revision_ordinal =
        record_revision(transaction, version, std::to_string(implementation_->version.number),
                        exact_file_sha256, sql);
    if (parsed.units.empty()) {
      complete_empty_migration(transaction, version, exact_file_sha256);
      return MigrationApplyResult{
          .already_completed = false,
          .completed_unit_count = 0U,
          .completed_file_sha256 = std::string{exact_file_sha256},
      };
    }

    for (std::size_t index = 0U; index < result.completed_unit_count; ++index) {
      const auto& unit = parsed.units[index];
      if (!replayable_session_unit(parsed, unit)) {
        continue;
      }
      const auto before_schema_sha256 = catalog_fingerprint(transaction);
      execute_plain_unit(transaction, parsed, unit);
      if (catalog_fingerprint(transaction) != before_schema_sha256) {
        throw Error{ErrorCode::drift,
                    "replayed PostgreSQL session state unexpectedly changed the schema"};
      }
    }

    for (std::size_t index = result.completed_unit_count; index < parsed.units.size(); ++index) {
      const auto& unit = parsed.units[index];
      const auto final_unit = index + 1U == parsed.units.size();
      const auto before_schema_sha256 = catalog_fingerprint(transaction);
      if (replayable_session_unit(parsed, unit)) {
        execute_plain_unit(transaction, parsed, unit);
        const auto after_schema_sha256 = catalog_fingerprint(transaction);
        if (after_schema_sha256 != before_schema_sha256) {
          throw Error{ErrorCode::drift,
                      "PostgreSQL session state unexpectedly changed the persistent schema"};
        }
        checkpoint_completed_unit(transaction, version, unit, before_schema_sha256,
                                  after_schema_sha256, revision_ordinal, final_unit,
                                  exact_file_sha256);
      } else if (unit.explicit_transaction) {
        execute_transaction_unit(transaction, parsed, unit, [&] {
          const auto after_schema_sha256 = catalog_fingerprint(transaction);
          checkpoint_completed_unit(transaction, version, unit, before_schema_sha256,
                                    after_schema_sha256, revision_ordinal, final_unit,
                                    exact_file_sha256);
        });
      } else {
        checkpoint_started_unit(transaction, version, unit, before_schema_sha256, revision_ordinal);
        try {
          execute_plain_unit(transaction, parsed, unit);
        } catch (const pqxx::sql_error&) {
          try {
            remove_failed_started_unit(transaction, version, unit);
          } catch (const std::exception& cleanup_error) {
            static_cast<void>(cleanup_error);
          }
          throw Error{ErrorCode::execution,
                      "PostgreSQL standalone migration unit failed before completion"};
        }
        const auto after_schema_sha256 = catalog_fingerprint(transaction);
        complete_started_unit(transaction, version, unit, after_schema_sha256, final_unit,
                              exact_file_sha256);
      }
      result.completed_unit_count = index + 1U;
    }
    result.completed_file_sha256 = exact_file_sha256;
    return result;
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::execution,
                "PostgreSQL migration apply failed; inspect history before retrying"};
  }
}

std::vector<MigrationRevision> Database::recover_revisions(const std::string_view version) const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  require_version(version);
  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    if (!history_tables_initialized(transaction)) {
      return {};
    }
    const auto rows =
        execute_parameters(transaction,
                           "SELECT ordinal, exact_file_sha256, sql "
                           "FROM \"_dbdiff\".\"revisions\" WHERE version = $1 ORDER BY ordinal",
                           version);
    std::vector<MigrationRevision> revisions;
    revisions.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
      auto exact_hash = row[1].as<std::string>();
      validate_stored_hash(exact_hash, "revision file hash");
      const auto ordinal = history_ordinal(row[0], "migration revision ordinal");
      auto exact_sql = row[2].as<std::string>();
      if (ordinal != revisions.size() || sha256_hex(exact_sql) != exact_hash) {
        throw Error{ErrorCode::drift,
                    "PostgreSQL migration revisions are not exact and contiguous"};
      }
      revisions.push_back(MigrationRevision{
          .ordinal = ordinal,
          .exact_file_sha256 = std::move(exact_hash),
          .sql = std::move(exact_sql),
      });
    }
    return revisions;
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::database, "PostgreSQL migration revision recovery failed"};
  }
}

} // namespace dbdiff::postgresql
