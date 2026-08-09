#include "dbdiff/lifecycle.hpp"

#include "dbdiff/docker.hpp"
#include "dbdiff/error.hpp"
#include "dbdiff/migration.hpp"
#include "dbdiff/postgresql.hpp"
#include "dbdiff/script.hpp"
#include "dbdiff/sqlite.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dbdiff {
namespace {

struct ProjectInputs {
  Config config;
  SourceSet sources;
  std::vector<MigrationFile> migrations;
};

sqlite::ConnectionSettings sqlite_settings(const Config& config) {
  return sqlite::ConnectionSettings{config.lock_timeout, config.statement_timeout};
}

postgresql::ConnectionSettings postgresql_settings(const Config& config) {
  return postgresql::ConnectionSettings{config.lock_timeout, config.statement_timeout};
}

[[noreturn]] void lifecycle_error(const ErrorCode code, const std::string& message) {
  throw Error{code, message};
}

ProjectInputs load_inputs(const std::filesystem::path& config_file, const Runtime& runtime) {
  auto config = load_config(config_file);
  SourceResolver resolver{config.file.parent_path(), config.migrations, runtime.stdin_reader};
  auto sources = resolver.resolve(config.sources);
  auto migrations = load_migrations(config.migrations, config.backend);
  return ProjectInputs{std::move(config), std::move(sources), std::move(migrations)};
}

std::string require_target_locator(const Config& config, const Runtime& runtime) {
  if (config.database.empty()) {
    lifecycle_error(ErrorCode::configuration,
                    "this command requires database or database_env in the configuration");
  }
  const auto locator = resolve_locator(config.database, runtime.environment);
  if (!locator || locator->empty()) {
    lifecycle_error(ErrorCode::configuration, "configured database locator is unavailable");
  }
  return *locator;
}

std::filesystem::path sqlite_target_path(const Config& config, const Runtime& runtime) {
  const auto locator = require_target_locator(config, runtime);
  constexpr std::string_view prefix{"sqlite:"};
  if (!std::string_view{locator}.starts_with(prefix)) {
    lifecycle_error(ErrorCode::configuration, "SQLite database locators must start with 'sqlite:'");
  }
  const auto path_text = std::string_view{locator}.substr(prefix.size());
  if (path_text.empty() || path_text == ":memory:" || path_text.starts_with("file:") ||
      path_text.find_first_of("?#") != std::string_view::npos) {
    lifecycle_error(ErrorCode::configuration,
                    "SQLite target must be a plain persistent filesystem path");
  }
  std::filesystem::path path{path_text};
  if (!path.is_absolute()) {
    path = config.file.parent_path() / path;
  }
  return path.lexically_normal();
}

bool sqlite_target_exists(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found) {
    return false;
  }
  if (error) {
    lifecycle_error(ErrorCode::database, "cannot inspect SQLite target path: " + error.message());
  }
  if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
    lifecycle_error(ErrorCode::database,
                    "SQLite target must be a regular file and not a symbolic link");
  }
  return true;
}

std::string concatenate_sources(const SourceSet& sources) {
  std::string sql;
  for (const auto& source : sources.files) {
    sql.push_back('\n');
    sql.append(source.sql);
    if (!sql.ends_with('\n')) {
      sql.push_back('\n');
    }
  }
  return sql;
}

struct AppliedPrefix {
  std::size_t completed_versions{0};
  std::optional<std::size_t> incomplete_unit_count;

  bool operator==(const AppliedPrefix&) const = default;
};

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

    if (entry.completed_file_sha256.has_value()) {
      if (saw_started || completed_hashes.size() != parsed.units.size() ||
          *entry.completed_file_sha256 != migration.exact_sha256 ||
          entry.attempted_file_sha256 != *entry.completed_file_sha256) {
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

struct SqlitePrefixReconstruction {
  AppliedPrefix prefix;
  sqlite::SchemaSnapshot snapshot;
};

SqlitePrefixReconstruction reconstruct_sqlite_prefix(const sqlite::MigrationHistory& history,
                                                     const std::vector<MigrationFile>& migrations,
                                                     sqlite::Database& database) {
  const auto prefix =
      validate_applied_history(history, migrations, BackendKind::sqlite, parse_sqlite_migration,
                               [](const sqlite::MigrationUnitRecord& unit) {
                                 return unit.state == sqlite::MigrationUnitState::completed;
                               });

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

int docker_major_from_image(const std::string_view image) {
  const auto colon = image.rfind(':');
  if (colon == std::string_view::npos) {
    return 18;
  }
  const auto tag = image.substr(colon + 1U);
  for (const int major : {15, 16, 17, 18}) {
    const auto text = std::to_string(major);
    if (tag == text || tag.starts_with(text + "-") || tag.starts_with(text + ".")) {
      return major;
    }
  }
  return 18;
}

struct PostgresProvisioning {
  std::string locator;
  std::optional<docker::PostgresContainer> container;
};

PostgresProvisioning provision_postgresql(const Config& config, const Runtime& runtime) {
  if (!config.scratch.locator.empty()) {
    const auto locator = resolve_locator(config.scratch.locator, runtime.environment);
    if (!locator) {
      lifecycle_error(ErrorCode::configuration, "PostgreSQL scratch locator is missing");
    }
    return PostgresProvisioning{*locator, std::nullopt};
  }
  if (config.scratch.docker) {
    docker::PostgresContainerOptions options;
    options.image = config.scratch.docker->image;
    options.postgres_major = docker_major_from_image(config.scratch.docker->image);
    auto container = docker::PostgresContainer::create(std::move(options));
    auto locator = container.connection_dsn();
    return PostgresProvisioning{std::move(locator), std::move(container)};
  }
  lifecycle_error(ErrorCode::configuration,
                  "PostgreSQL create requires scratch.database, scratch.database_env, or "
                  "scratch.docker");
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

struct PostgresPrefixReconstruction {
  AppliedPrefix prefix;
  postgresql::SchemaSnapshot snapshot;
};

PostgresPrefixReconstruction reconstruct_postgresql_prefix(
    const postgresql::MigrationHistory& history, const std::vector<MigrationFile>& migrations,
    postgresql::ScratchDatabase& scratch, const std::vector<std::string>& managed_schemas) {
  const auto prefix = validate_applied_history(
      history, migrations, BackendKind::postgresql, postgresql::parse_migration,
      [](const postgresql::MigrationUnitRecord& unit) {
        return unit.state == postgresql::MigrationUnitState::completed;
      });

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
    require_migration_start(migration, snapshot.semantic_hash);
    scratch.execute_prefix(migration.sql, *prefix.incomplete_unit_count);
    snapshot = scratch.introspect(managed_schemas);
  }
  return PostgresPrefixReconstruction{prefix, std::move(snapshot)};
}

MigrationMetadata metadata_for(const Config& config, const std::string& version,
                               const std::string& engine_version, const std::string& from_hash,
                               const std::string& to_hash, const SourceSet& sources,
                               const HazardSet& hazards, const bool draft) {
  return MigrationMetadata{1,       config.backend,       version, engine_version, from_hash,
                           to_hash, sources.exact_sha256, hazards, draft};
}

void save_candidate(const Config& config, const MigrationMetadata& metadata,
                    const std::string_view body, CreateResult& result) {
  auto sql = render_migration_metadata(metadata);
  sql.append(body);
  if (!sql.ends_with('\n')) {
    sql.push_back('\n');
  }
  save_migration_atomic(config.migrations, metadata.version, sql);
  result.created = true;
  result.draft = metadata.draft;
  result.file = config.migrations / (metadata.version + ".sql");
  result.version = metadata.version;
  result.hazards = metadata.allowed_hazards;
}

int parse_timestamp_field(const std::string_view value, const std::string_view name) {
  int result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    lifecycle_error(ErrorCode::migration,
                    "last migration has an invalid " + std::string{name} + " timestamp field");
  }
  return result;
}

std::chrono::system_clock::time_point next_time_after_version(const std::string_view version) {
  if (version.size() < 14U) {
    lifecycle_error(ErrorCode::migration, "last migration version has no UTC timestamp");
  }
  const auto year_value = parse_timestamp_field(version.substr(0, 4), "year");
  const auto month_value = parse_timestamp_field(version.substr(4, 2), "month");
  const auto day_value = parse_timestamp_field(version.substr(6, 2), "day");
  const auto hour_value = parse_timestamp_field(version.substr(8, 2), "hour");
  const auto minute_value = parse_timestamp_field(version.substr(10, 2), "minute");
  const auto second_value = parse_timestamp_field(version.substr(12, 2), "second");
  const std::chrono::year_month_day date{std::chrono::year{year_value},
                                         std::chrono::month{static_cast<unsigned>(month_value)},
                                         std::chrono::day{static_cast<unsigned>(day_value)}};
  if (!date.ok() || hour_value < 0 || hour_value > 23 || minute_value < 0 || minute_value > 59 ||
      second_value < 0 || second_value > 59) {
    lifecycle_error(ErrorCode::migration, "last migration version has an invalid UTC timestamp");
  }
  return std::chrono::sys_days{date} + std::chrono::hours{hour_value} +
         std::chrono::minutes{minute_value} + std::chrono::seconds{second_value + 1};
}

std::string next_migration_version(const std::chrono::system_clock::time_point now,
                                   const std::string_view name,
                                   const std::vector<MigrationFile>& migrations) {
  auto version = make_migration_version(now, name);
  if (migrations.empty() || version > migrations.back().metadata.version) {
    return version;
  }
  version =
      make_migration_version(next_time_after_version(migrations.back().metadata.version), name);
  if (version <= migrations.back().metadata.version) {
    lifecycle_error(ErrorCode::migration,
                    "could not allocate a migration version after the existing stream");
  }
  return version;
}

CreateResult create_sqlite(ProjectInputs& project, const CreateOptions& options,
                           const Runtime& runtime) {
  auto history = sqlite::Database::temporary(sqlite_settings(project.config));
  const auto from = replay_sqlite(project.migrations, history);

  auto master = sqlite::Database::temporary(sqlite_settings(project.config));
  master.execute_source(concatenate_sources(project.sources));
  const auto to = master.inspect();
  if (from.semantic_hash == to.semantic_hash) {
    return {};
  }

  const auto candidate = sqlite::plan(from, to);
  require_hazard_approvals(candidate.hazards, options.allowed_hazards);
  if (candidate.sql.empty()) {
    lifecycle_error(ErrorCode::unsupported,
                    "SQLite schemas differ but no verifiable migration could be rendered");
  }
  history.execute_migration(candidate.sql);
  if (history.inspect().semantic_hash != to.semantic_hash) {
    lifecycle_error(ErrorCode::migration,
                    "SQLite candidate did not converge reconstructed history to master");
  }

  CreateResult result;
  const auto version = next_migration_version(runtime.now(), options.name, project.migrations);
  const auto metadata =
      metadata_for(project.config, version, sqlite3_libversion(), from.semantic_hash,
                   to.semantic_hash, project.sources, candidate.hazards, candidate.draft);
  save_candidate(project.config, metadata, candidate.sql, result);
  return result;
}

CreateResult create_postgresql(ProjectInputs& project, const CreateOptions& options,
                               const Runtime& runtime) {
  auto provisioning = provision_postgresql(project.config, runtime);
  auto history = postgresql::ScratchDatabase::create(provisioning.locator,
                                                     postgresql_settings(project.config));
  auto master = postgresql::ScratchDatabase::create(provisioning.locator,
                                                    postgresql_settings(project.config));

  const auto from = replay_postgresql(project.migrations, history, project.config.managed_schemas);
  master.execute_sources(project.sources);
  const auto to = master.introspect(project.config.managed_schemas);
  if (from.server_version.major != to.server_version.major) {
    lifecycle_error(ErrorCode::unsupported,
                    "PostgreSQL history and master scratch databases have different majors");
  }
  if (from.semantic_hash == to.semantic_hash) {
    master.close();
    history.close();
    if (provisioning.container) {
      provisioning.container->close();
    }
    return {};
  }

  const auto candidate = postgresql::plan(from, to);
  const auto hazards = collect_hazards(candidate.operations);
  require_hazard_approvals(hazards, options.allowed_hazards);
  const auto body = postgresql::render_plan(candidate);
  if (body.empty()) {
    lifecycle_error(ErrorCode::unsupported,
                    "PostgreSQL schemas differ but no verifiable migration could be rendered");
  }
  history.execute_migration(body);
  if (history.introspect(project.config.managed_schemas).semantic_hash != to.semantic_hash) {
    lifecycle_error(ErrorCode::migration,
                    "PostgreSQL candidate did not converge reconstructed history to master");
  }

  CreateResult result;
  const auto version = next_migration_version(runtime.now(), options.name, project.migrations);
  const auto metadata =
      metadata_for(project.config, version, std::to_string(from.server_version.number),
                   from.semantic_hash, to.semantic_hash, project.sources, hazards, candidate.draft);
  save_candidate(project.config, metadata, body, result);

  master.close();
  history.close();
  if (provisioning.container) {
    provisioning.container->close();
  }
  return result;
}

sqlite::SchemaSnapshot require_complete_sqlite_reconstruction(ProjectInputs& project) {
  auto history = sqlite::Database::temporary(sqlite_settings(project.config));
  const auto reconstructed = replay_sqlite(project.migrations, history);
  auto master = sqlite::Database::temporary(sqlite_settings(project.config));
  master.execute_source(concatenate_sources(project.sources));
  const auto desired = master.inspect();
  if (reconstructed.semantic_hash != desired.semantic_hash) {
    lifecycle_error(ErrorCode::migration,
                    "complete SQLite migration stream does not reproduce the master schema");
  }
  return reconstructed;
}

std::size_t apply_sqlite_pending(sqlite::Database& database,
                                 const std::vector<MigrationFile>& migrations,
                                 const AppliedPrefix& prefix, const bool resume) {
  if (prefix.incomplete_unit_count.has_value() && !resume) {
    lifecycle_error(ErrorCode::migration,
                    "the last migration is incomplete; inspect it and apply with --resume");
  }
  if (!prefix.incomplete_unit_count.has_value() && resume) {
    lifecycle_error(ErrorCode::configuration,
                    "--resume requires an incomplete migration in database history");
  }

  std::size_t applied = 0;
  auto current = database.inspect();
  for (std::size_t index = prefix.completed_versions; index < migrations.size(); ++index) {
    const auto& migration = migrations[index];
    const bool resume_version =
        prefix.incomplete_unit_count.has_value() && index == prefix.completed_versions;
    if (!resume_version) {
      require_migration_start(migration, current.semantic_hash);
    }
    const auto result = database.apply_version(migration.metadata.version, migration.exact_sha256,
                                               migration.sql, resume_version);
    current = database.inspect();
    require_migration_end(migration, current.semantic_hash);
    if (!result.already_completed) {
      ++applied;
    }
  }
  return applied;
}

ApplyResult apply_sqlite(ProjectInputs& project, const ApplyOptions& options,
                         const Runtime& runtime) {
  const auto desired = require_complete_sqlite_reconstruction(project);
  const auto path = sqlite_target_path(project.config, runtime);
  const bool existed = sqlite_target_exists(path);
  if (!existed && !options.create_database) {
    lifecycle_error(ErrorCode::database,
                    "SQLite target does not exist; pass --create-database to create it");
  }

  std::optional<sqlite::Database> target;
  if (existed) {
    target.emplace(sqlite::Database::open(
        path, options.dry_run ? sqlite::OpenMode::read_only : sqlite::OpenMode::read_write,
        sqlite_settings(project.config)));
  }

  const auto history = target ? target->read_history() : sqlite::MigrationHistory{};
  auto prefix_database = sqlite::Database::temporary(sqlite_settings(project.config));
  const auto prefix = reconstruct_sqlite_prefix(history, project.migrations, prefix_database);
  const auto live = target ? target->inspect()
                           : sqlite::Database::temporary(sqlite_settings(project.config)).inspect();
  if (live.semantic_hash != prefix.snapshot.semantic_hash) {
    lifecycle_error(ErrorCode::drift,
                    "live SQLite schema differs from its reconstructed migration prefix");
  }

  const auto pending = project.migrations.size() - prefix.prefix.completed_versions;
  if (prefix.prefix.incomplete_unit_count.has_value() && !options.resume) {
    lifecycle_error(ErrorCode::migration,
                    "the last migration is incomplete; inspect it and apply with --resume");
  }
  if (!prefix.prefix.incomplete_unit_count.has_value() && options.resume) {
    lifecycle_error(ErrorCode::configuration,
                    "--resume requires an incomplete migration in database history");
  }

  if (options.validate_data) {
    auto validation = sqlite::Database::temporary(sqlite_settings(project.config));
    if (target) {
      target->backup_to(validation);
    }
    static_cast<void>(
        apply_sqlite_pending(validation, project.migrations, prefix.prefix, options.resume));
    if (validation.inspect().semantic_hash != desired.semantic_hash) {
      lifecycle_error(ErrorCode::migration,
                      "data-bearing SQLite validation did not converge to reconstructed history");
    }
  }

  if (options.dry_run) {
    return ApplyResult{pending, 0, true};
  }
  if (!target) {
    target.emplace(sqlite::Database::open(path, sqlite::OpenMode::read_write_create,
                                          sqlite_settings(project.config)));
  }

  const auto repeated_history = target->read_history();
  auto repeated_prefix_database = sqlite::Database::temporary(sqlite_settings(project.config));
  const auto repeated =
      reconstruct_sqlite_prefix(repeated_history, project.migrations, repeated_prefix_database);
  if (repeated.prefix != prefix.prefix ||
      target->inspect().semantic_hash != repeated.snapshot.semantic_hash) {
    lifecycle_error(ErrorCode::drift,
                    "SQLite target changed after validation and before migration execution");
  }

  const auto applied =
      apply_sqlite_pending(*target, project.migrations, repeated.prefix, options.resume);
  if (target->inspect().semantic_hash != desired.semantic_hash) {
    lifecycle_error(ErrorCode::migration,
                    "applied SQLite target does not match reconstructed migration history");
  }
  return ApplyResult{pending, applied, false};
}

StatusResult status_sqlite(ProjectInputs& project, const Runtime& runtime) {
  static_cast<void>(require_complete_sqlite_reconstruction(project));
  const auto path = sqlite_target_path(project.config, runtime);
  if (!sqlite_target_exists(path)) {
    return StatusResult{ProjectStatus::missing_database, 0, project.migrations.size(),
                        "SQLite database is missing"};
  }

  auto target =
      sqlite::Database::open(path, sqlite::OpenMode::read_only, sqlite_settings(project.config));
  auto prefix_database = sqlite::Database::temporary(sqlite_settings(project.config));
  const auto prefix =
      reconstruct_sqlite_prefix(target.read_history(), project.migrations, prefix_database);
  if (target.inspect().semantic_hash != prefix.snapshot.semantic_hash) {
    return StatusResult{ProjectStatus::drift, prefix.prefix.completed_versions,
                        project.migrations.size(),
                        "Live SQLite schema has drifted from its recorded migration prefix"};
  }
  if (prefix.prefix.completed_versions != project.migrations.size() ||
      prefix.prefix.incomplete_unit_count.has_value()) {
    return StatusResult{ProjectStatus::pending, prefix.prefix.completed_versions,
                        project.migrations.size(), "SQLite migrations are pending"};
  }
  return StatusResult{ProjectStatus::converged, prefix.prefix.completed_versions,
                      project.migrations.size(), "SQLite schema is converged"};
}

postgresql::SchemaSnapshot
require_complete_postgresql_reconstruction(ProjectInputs& project,
                                           PostgresProvisioning& provisioning) {
  auto history = postgresql::ScratchDatabase::create(provisioning.locator,
                                                     postgresql_settings(project.config));
  const auto reconstructed =
      replay_postgresql(project.migrations, history, project.config.managed_schemas);
  auto master = postgresql::ScratchDatabase::create(provisioning.locator,
                                                    postgresql_settings(project.config));
  master.execute_sources(project.sources);
  const auto desired = master.introspect(project.config.managed_schemas);
  if (reconstructed.semantic_hash != desired.semantic_hash) {
    lifecycle_error(ErrorCode::migration,
                    "complete PostgreSQL migration stream does not reproduce the master schema");
  }
  return reconstructed;
}

std::size_t apply_postgresql_pending(postgresql::Database& database,
                                     const std::vector<MigrationFile>& migrations,
                                     const AppliedPrefix& prefix, const bool resume,
                                     const std::vector<std::string>& managed_schemas) {
  if (prefix.incomplete_unit_count.has_value() && !resume) {
    lifecycle_error(ErrorCode::migration,
                    "the last migration is incomplete; inspect it and apply with --resume");
  }
  if (!prefix.incomplete_unit_count.has_value() && resume) {
    lifecycle_error(ErrorCode::configuration,
                    "--resume requires an incomplete migration in database history");
  }

  std::size_t applied = 0;
  auto current = database.introspect(managed_schemas);
  for (std::size_t index = prefix.completed_versions; index < migrations.size(); ++index) {
    const auto& migration = migrations[index];
    const bool resume_version =
        prefix.incomplete_unit_count.has_value() && index == prefix.completed_versions;
    if (!resume_version) {
      require_migration_start(migration, current.semantic_hash);
    }
    const auto result = database.apply_version(migration.metadata.version, migration.exact_sha256,
                                               migration.sql, resume_version);
    current = database.introspect(managed_schemas);
    require_migration_end(migration, current.semantic_hash);
    if (!result.already_completed) {
      ++applied;
    }
  }
  return applied;
}

ApplyResult apply_postgresql(ProjectInputs& project, const ApplyOptions& options,
                             const Runtime& runtime) {
  if (options.create_database || options.validate_data) {
    lifecycle_error(ErrorCode::configuration,
                    "--create-database and --validate-data are SQLite-only options");
  }
  auto provisioning = provision_postgresql(project.config, runtime);
  const auto desired = require_complete_postgresql_reconstruction(project, provisioning);

  auto target = postgresql::Database::open(require_target_locator(project.config, runtime),
                                           postgresql_settings(project.config));
  if (target.server_version().major != desired.server_version.major) {
    lifecycle_error(ErrorCode::unsupported,
                    "target and scratch PostgreSQL major versions must match");
  }
  const auto history = target.read_history();
  auto prefix_database = postgresql::ScratchDatabase::create(provisioning.locator,
                                                             postgresql_settings(project.config));
  const auto prefix = reconstruct_postgresql_prefix(history, project.migrations, prefix_database,
                                                    project.config.managed_schemas);
  if (target.introspect(project.config.managed_schemas).semantic_hash !=
      prefix.snapshot.semantic_hash) {
    lifecycle_error(ErrorCode::drift,
                    "live PostgreSQL schema differs from its reconstructed migration prefix");
  }

  const auto pending = project.migrations.size() - prefix.prefix.completed_versions;
  if (prefix.prefix.incomplete_unit_count.has_value() && !options.resume) {
    lifecycle_error(ErrorCode::migration,
                    "the last migration is incomplete; inspect it and apply with --resume");
  }
  if (!prefix.prefix.incomplete_unit_count.has_value() && options.resume) {
    lifecycle_error(ErrorCode::configuration,
                    "--resume requires an incomplete migration in database history");
  }
  if (options.dry_run) {
    return ApplyResult{pending, 0, true};
  }

  const auto repeated_history = target.read_history();
  auto repeated_prefix_database = postgresql::ScratchDatabase::create(
      provisioning.locator, postgresql_settings(project.config));
  const auto repeated =
      reconstruct_postgresql_prefix(repeated_history, project.migrations, repeated_prefix_database,
                                    project.config.managed_schemas);
  if (repeated.prefix != prefix.prefix ||
      target.introspect(project.config.managed_schemas).semantic_hash !=
          repeated.snapshot.semantic_hash) {
    lifecycle_error(ErrorCode::drift,
                    "PostgreSQL target changed after validation and before migration execution");
  }

  const auto applied = apply_postgresql_pending(target, project.migrations, repeated.prefix,
                                                options.resume, project.config.managed_schemas);
  if (target.introspect(project.config.managed_schemas).semantic_hash != desired.semantic_hash) {
    lifecycle_error(ErrorCode::migration,
                    "applied PostgreSQL target does not match reconstructed migration history");
  }
  return ApplyResult{pending, applied, false};
}

StatusResult status_postgresql(ProjectInputs& project, const Runtime& runtime) {
  auto provisioning = provision_postgresql(project.config, runtime);
  const auto desired = require_complete_postgresql_reconstruction(project, provisioning);
  auto target = postgresql::Database::open(require_target_locator(project.config, runtime),
                                           postgresql_settings(project.config));
  if (target.server_version().major != desired.server_version.major) {
    lifecycle_error(ErrorCode::unsupported,
                    "target and scratch PostgreSQL major versions must match");
  }
  auto prefix_database = postgresql::ScratchDatabase::create(provisioning.locator,
                                                             postgresql_settings(project.config));
  const auto prefix = reconstruct_postgresql_prefix(
      target.read_history(), project.migrations, prefix_database, project.config.managed_schemas);
  if (target.introspect(project.config.managed_schemas).semantic_hash !=
      prefix.snapshot.semantic_hash) {
    return StatusResult{ProjectStatus::drift, prefix.prefix.completed_versions,
                        project.migrations.size(),
                        "Live PostgreSQL schema has drifted from its recorded migration prefix"};
  }
  if (prefix.prefix.completed_versions != project.migrations.size() ||
      prefix.prefix.incomplete_unit_count.has_value()) {
    return StatusResult{ProjectStatus::pending, prefix.prefix.completed_versions,
                        project.migrations.size(), "PostgreSQL migrations are pending"};
  }
  return StatusResult{ProjectStatus::converged, prefix.prefix.completed_versions,
                      project.migrations.size(), "PostgreSQL schema is converged"};
}

template <typename Revision>
std::vector<RecoveredRevision> convert_revisions(const std::vector<Revision>& revisions,
                                                 const std::string_view version) {
  if (revisions.empty()) {
    lifecycle_error(ErrorCode::migration, "database has no stored revisions for migration '" +
                                              std::string{version} + "'");
  }
  std::vector<RecoveredRevision> result;
  result.reserve(revisions.size());
  for (const auto& revision : revisions) {
    result.push_back(RecoveredRevision{revision.ordinal, revision.exact_file_sha256, revision.sql});
  }
  return result;
}

} // namespace

Runtime default_runtime() {
  return Runtime{
      .environment = [](const std::string_view name) -> std::optional<std::string> {
        const auto key = std::string{name};
        const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe)
        if (value == nullptr) {
          return std::nullopt;
        }
        return std::string{value};
      },
      .stdin_reader =
          [] {
            std::ostringstream input;
            input << std::cin.rdbuf();
            return input.str();
          },
      .now = [] { return std::chrono::system_clock::now(); },
  };
}

std::string make_migration_version(const std::chrono::system_clock::time_point time,
                                   const std::string_view name) {
  std::string slug;
  bool separator = false;
  for (const char raw_character : name) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isalnum(character) != 0 && character < 128U) {
      if (separator && !slug.empty()) {
        slug.push_back('_');
      }
      slug.push_back(static_cast<char>(std::tolower(character)));
      separator = false;
    } else {
      separator = true;
    }
  }
  if (slug.empty()) {
    lifecycle_error(ErrorCode::configuration,
                    "migration name must contain at least one ASCII letter or digit");
  }

  const auto raw_time = std::chrono::system_clock::to_time_t(time);
  std::tm utc{};
  if (::gmtime_r(&raw_time, &utc) == nullptr) {
    lifecycle_error(ErrorCode::configuration, "migration time is outside the supported range");
  }
  std::array<char, 32> timestamp{};
  if (std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d%H%M%S", &utc) == 0) {
    lifecycle_error(ErrorCode::configuration, "could not format migration timestamp");
  }
  return std::string{timestamp.data()} + "_" + slug;
}

HazardSet collect_hazards(const std::vector<Operation>& operations) {
  HazardSet result;
  for (const auto& operation : operations) {
    result.insert(operation.hazards.begin(), operation.hazards.end());
  }
  return result;
}

void require_hazard_approvals(const HazardSet& required, const HazardSet& allowed) {
  std::vector<std::string> missing;
  for (const auto hazard : required) {
    if (!allowed.contains(hazard)) {
      missing.emplace_back(hazard_name(hazard));
    }
  }
  if (!missing.empty()) {
    std::string message{"migration requires --allow-hazard"};
    for (const auto& hazard : missing) {
      message.append(" ");
      message.append(hazard);
    }
    lifecycle_error(ErrorCode::migration, message);
  }
}

CreateResult create_migration(const CreateOptions& options, const Runtime& runtime) {
  auto project = load_inputs(options.config_file, runtime);
  if (project.config.backend == BackendKind::sqlite) {
    return create_sqlite(project, options, runtime);
  }
  return create_postgresql(project, options, runtime);
}

ApplyResult apply_migrations(const ApplyOptions& options, const Runtime& runtime) {
  auto project = load_inputs(options.config_file, runtime);
  if (project.config.backend == BackendKind::sqlite) {
    return apply_sqlite(project, options, runtime);
  }
  return apply_postgresql(project, options, runtime);
}

StatusResult project_status(const std::filesystem::path& config_file, const Runtime& runtime) {
  auto project = load_inputs(config_file, runtime);
  if (project.config.backend == BackendKind::sqlite) {
    return status_sqlite(project, runtime);
  }
  return status_postgresql(project, runtime);
}

std::vector<RecoveredRevision> recover_migration(const RecoverOptions& options,
                                                 const Runtime& runtime) {
  if (options.version.empty()) {
    lifecycle_error(ErrorCode::configuration, "recover requires a migration version");
  }
  const auto config = load_config(options.config_file);
  if (config.backend == BackendKind::sqlite) {
    const auto path = sqlite_target_path(config, runtime);
    if (!sqlite_target_exists(path)) {
      lifecycle_error(ErrorCode::database, "SQLite target does not exist");
    }
    const auto target =
        sqlite::Database::open(path, sqlite::OpenMode::read_only, sqlite_settings(config));
    return convert_revisions(target.recover_revisions(options.version), options.version);
  }

  const auto target = postgresql::Database::open(require_target_locator(config, runtime),
                                                 postgresql_settings(config));
  return convert_revisions(target.recover_revisions(options.version), options.version);
}

} // namespace dbdiff
