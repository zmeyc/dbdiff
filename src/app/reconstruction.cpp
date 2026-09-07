#include "project.hpp"

#include "dbdiff/script.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbdiff::app_detail {

template <typename History, typename ParseMigration, typename IsCompleted>
AppliedPrefix validate_applied_history(const History& history,
                                       const std::vector<MigrationFile>& migrations,
                                       const BackendKind backend, ParseMigration parse_migration,
                                       IsCompleted is_completed) {
  if (!history.initialized) {
    if (!history.entries.empty()) {
      lifecycle_error(ErrorCode::database,
                      "migration history has rows but its storage is not initialized");
    }
    return {};
  }
  if (history.entries.size() > migrations.size()) {
    lifecycle_error(ErrorCode::drift,
                    "database history contains migrations absent from the migration directory");
  }

  AppliedPrefix prefix;
  for (std::size_t migration_index = 0; migration_index < history.entries.size();
       ++migration_index) {
    const auto& entry = history.entries[migration_index];
    const auto& migration = migrations[migration_index];
    if (entry.version != migration.metadata.version) {
      lifecycle_error(ErrorCode::drift,
                      "database migration history is not a prefix of the migration directory");
    }
    if (entry.backend != backend_name(backend)) {
      lifecycle_error(ErrorCode::drift,
                      "database migration history was written by a different backend");
    }
    if (entry.engine_version.empty() || entry.attempted_file_sha256.size() != 64U) {
      lifecycle_error(ErrorCode::database,
                      "database migration history contains invalid immutable metadata");
    }
    try {
      validate_engine_version(backend, entry.engine_version);
    } catch (const Error&) {
      lifecycle_error(ErrorCode::database,
                      "database migration history contains an invalid engine version");
    }
    if (backend == BackendKind::postgresql &&
        postgresql::parse_server_version(entry.engine_version).major !=
            postgresql::parse_server_version(migration.metadata.engine_version).major) {
      lifecycle_error(ErrorCode::drift,
                      "database migration history records another PostgreSQL major");
    }

    const auto parsed = parse_migration(migration.sql);
    std::vector<std::string> completed_hashes;
    bool saw_started = false;
    for (std::size_t unit_index = 0; unit_index < entry.units.size(); ++unit_index) {
      const auto& unit = entry.units[unit_index];
      if (unit.ordinal != unit_index) {
        lifecycle_error(ErrorCode::database,
                        "database migration units are not a contiguous ordered prefix");
      }
      if (!is_completed(unit)) {
        if (saw_started || unit_index + 1U != entry.units.size()) {
          lifecycle_error(ErrorCode::database,
                          "database migration history contains an invalid started unit");
        }
        saw_started = true;
        continue;
      }
      if (saw_started || unit_index >= parsed.units.size()) {
        lifecycle_error(ErrorCode::database,
                        "database migration history contains an invalid completed unit");
      }
      if (unit.explicit_transaction != parsed.units[unit_index].explicit_transaction) {
        lifecycle_error(ErrorCode::migration,
                        "edited migration changes a completed transaction boundary");
      }
      completed_hashes.push_back(unit.exact_sha256);
    }
    validate_completed_prefix(completed_hashes, parsed);

    if (const auto completed_file_sha256 = entry.completed_file_sha256;
        completed_file_sha256.has_value()) {
      if (saw_started || completed_hashes.size() != parsed.units.size() ||
          *completed_file_sha256 != migration.exact_sha256 ||
          entry.attempted_file_sha256 != *completed_file_sha256) {
        lifecycle_error(ErrorCode::migration, "completed migration '" + migration.metadata.version +
                                                  "' differs from its database checksum");
      }
      ++prefix.completed_versions;
      continue;
    }

    if (migration_index + 1U != history.entries.size()) {
      lifecycle_error(ErrorCode::database,
                      "an incomplete migration is followed by another history entry");
    }
    prefix.incomplete_unit_count = completed_hashes.size();
  }
  return prefix;
}

ParsedScript parse_sqlite_migration(std::string sql) {
  auto statements = sqlite::scan_statements(sql);
  return build_execution_units(std::move(sql), std::move(statements));
}

void require_migration_start(const MigrationFile& migration, const std::string_view before) {
  if (migration.metadata.draft) {
    lifecycle_error(ErrorCode::migration,
                    "cannot reconstruct draft migration '" + migration.metadata.version + "'");
  }
  if (migration.metadata.from_sha256 != before) {
    lifecycle_error(ErrorCode::migration, "migration '" + migration.metadata.version +
                                              "' does not start from the reconstructed schema");
  }
}

void require_migration_end(const MigrationFile& migration, const std::string_view after) {
  if (migration.metadata.to_sha256 != after) {
    lifecycle_error(ErrorCode::migration, "migration '" + migration.metadata.version +
                                              "' does not produce its declared schema");
  }
}

sqlite::SchemaSnapshot replay_sqlite(const std::vector<MigrationFile>& migrations,
                                     sqlite::Database& database) {
  auto snapshot = database.inspect();
  for (const auto& migration : migrations) {
    require_migration_start(migration, snapshot.semantic_hash);
    database.execute_migration(migration.sql);
    snapshot = database.inspect();
    require_migration_end(migration, snapshot.semantic_hash);
  }
  return snapshot;
}

AppliedPrefix validate_sqlite_history(const sqlite::MigrationHistory& history,
                                      const std::vector<MigrationFile>& migrations) {
  return validate_applied_history(history, migrations, BackendKind::sqlite, parse_sqlite_migration,
                                  [](const sqlite::MigrationUnitRecord& unit) {
                                    return unit.state == sqlite::MigrationUnitState::completed;
                                  });
}

AppliedPrefix validate_postgresql_history(const postgresql::MigrationHistory& history,
                                          const std::vector<MigrationFile>& migrations) {
  return validate_applied_history(history, migrations, BackendKind::postgresql,
                                  postgresql::parse_migration,
                                  [](const postgresql::MigrationUnitRecord& unit) {
                                    return unit.state == postgresql::MigrationUnitState::completed;
                                  });
}

SqlitePrefixReconstruction reconstruct_sqlite_prefix(const sqlite::MigrationHistory& history,
                                                     const std::vector<MigrationFile>& migrations,
                                                     sqlite::Database& database,
                                                     const std::string_view live_schema_sha256) {
  auto prefix = validate_sqlite_history(history, migrations);
  if (prefix.incomplete_unit_count.has_value()) {
    const auto& entry = history.entries[prefix.completed_versions];
    if (!entry.units.empty() && entry.units.back().state == sqlite::MigrationUnitState::started) {
      const auto& started = entry.units.back();
      const auto resolution = sqlite::classify_started_unit(started, live_schema_sha256);
      if (resolution == sqlite::StartedUnitResolution::complete) {
        const auto parsed = parse_sqlite_migration(migrations[prefix.completed_versions].sql);
        if (started.ordinal >= parsed.units.size() ||
            started.exact_sha256 != parsed.units[started.ordinal].exact_sha256 ||
            started.explicit_transaction != parsed.units[started.ordinal].explicit_transaction) {
          lifecycle_error(ErrorCode::migration,
                          "cannot safely resume an edited standalone migration unit that may "
                          "already be applied");
        }
        ++*prefix.incomplete_unit_count;
      }
    }
  }

  auto snapshot = database.inspect();
  for (std::size_t index = 0; index < prefix.completed_versions; ++index) {
    const auto& migration = migrations[index];
    require_migration_start(migration, snapshot.semantic_hash);
    database.execute_migration(migration.sql);
    snapshot = database.inspect();
    require_migration_end(migration, snapshot.semantic_hash);
  }
  if (prefix.incomplete_unit_count.has_value()) {
    const auto& migration = migrations[prefix.completed_versions];
    require_migration_start(migration, snapshot.semantic_hash);
    database.execute_prefix(migration.sql, *prefix.incomplete_unit_count);
    snapshot = database.inspect();
  }
  return SqlitePrefixReconstruction{prefix, std::move(snapshot)};
}

postgresql::SchemaSnapshot replay_postgresql(const std::vector<MigrationFile>& migrations,
                                             postgresql::ScratchDatabase& database,
                                             const std::vector<std::string>& managed_schemas) {
  auto snapshot = database.introspect(managed_schemas);
  for (const auto& migration : migrations) {
    if (postgresql::parse_server_version(migration.metadata.engine_version).major !=
        snapshot.server_version.major) {
      lifecycle_error(ErrorCode::unsupported, "migration '" + migration.metadata.version +
                                                  "' targets a different PostgreSQL major");
    }
    require_migration_start(migration, snapshot.semantic_hash);
    database.execute_migration(migration.sql);
    snapshot = database.introspect(managed_schemas);
    require_migration_end(migration, snapshot.semantic_hash);
  }
  return snapshot;
}

PostgresPrefixReconstruction reconstruct_postgresql_prefix(
    const postgresql::MigrationHistory& history, const std::vector<MigrationFile>& migrations,
    postgresql::ScratchDatabase& scratch, const std::vector<std::string>& managed_schemas) {
  const auto prefix = validate_postgresql_history(history, migrations);

  auto snapshot = scratch.introspect(managed_schemas);
  for (std::size_t index = 0; index < prefix.completed_versions; ++index) {
    const auto& migration = migrations[index];
    if (postgresql::parse_server_version(migration.metadata.engine_version).major !=
        snapshot.server_version.major) {
      lifecycle_error(ErrorCode::unsupported, "migration '" + migration.metadata.version +
                                                  "' targets a different PostgreSQL major");
    }
    require_migration_start(migration, snapshot.semantic_hash);
    scratch.execute_migration(migration.sql);
    snapshot = scratch.introspect(managed_schemas);
    require_migration_end(migration, snapshot.semantic_hash);
  }
  if (prefix.incomplete_unit_count.has_value()) {
    const auto& migration = migrations[prefix.completed_versions];
    if (postgresql::parse_server_version(migration.metadata.engine_version).major !=
        snapshot.server_version.major) {
      lifecycle_error(ErrorCode::unsupported, "migration '" + migration.metadata.version +
                                                  "' targets a different PostgreSQL major");
    }
    require_migration_start(migration, snapshot.semantic_hash);
    scratch.execute_prefix(migration.sql, *prefix.incomplete_unit_count);
    snapshot = scratch.introspect(managed_schemas);
  }
  return PostgresPrefixReconstruction{prefix, std::move(snapshot)};
}

} // namespace dbdiff::app_detail
