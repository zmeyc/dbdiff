#include "dbdiff/lifecycle.hpp"

#include "project.hpp"

#include <sqlite3.h>

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbdiff {
using namespace app_detail;
namespace {

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
  const auto live = target ? target->inspect()
                           : sqlite::Database::temporary(sqlite_settings(project.config)).inspect();
  const auto prefix =
      reconstruct_sqlite_prefix(history, project.migrations, prefix_database, live.semantic_hash);
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
  const auto repeated_live = target->inspect();
  const auto repeated = reconstruct_sqlite_prefix(
      repeated_history, project.migrations, repeated_prefix_database, repeated_live.semantic_hash);
  if (repeated_history != history || repeated.prefix != prefix.prefix ||
      repeated_live.semantic_hash != repeated.snapshot.semantic_hash) {
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
  const auto lifecycle_lock = target.acquire_lifecycle_lock();
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
  auto config = load_config(options.config_file);
  const ProjectLifecycleLock lifecycle_lock{config.file, config.lock_timeout};
  auto project = load_inputs(std::move(config), runtime);
  if (project.config.backend == BackendKind::sqlite) {
    return create_sqlite(project, options, runtime);
  }
  return create_postgresql(project, options, runtime);
}

ApplyResult apply_migrations(const ApplyOptions& options, const Runtime& runtime) {
  auto config = load_config(options.config_file);
  const ProjectLifecycleLock lifecycle_lock{config.file, config.lock_timeout};
  auto project = load_inputs(std::move(config), runtime);
  if (project.config.backend == BackendKind::sqlite) {
    return apply_sqlite(project, options, runtime);
  }
  return apply_postgresql(project, options, runtime);
}

std::vector<RecoveredRevision> recover_migration(const RecoverOptions& options,
                                                 const Runtime& runtime) {
  if (options.version.empty()) {
    lifecycle_error(ErrorCode::configuration, "recover requires a migration version");
  }
  const auto config = load_config(options.config_file);
  const ProjectLifecycleLock lifecycle_lock{config.file, config.lock_timeout};
  if (config.backend == BackendKind::sqlite) {
    const auto path = sqlite_target_path(config, runtime);
    if (!sqlite_target_exists(path)) {
      lifecycle_error(ErrorCode::database, "SQLite target does not exist");
    }
    const auto target =
        sqlite::Database::open(path, sqlite::OpenMode::read_only, sqlite_settings(config));
    return convert_revisions(target.recover_revisions(options.version), options.version);
  }

  auto target = postgresql::Database::open(require_target_locator(config, runtime),
                                           postgresql_settings(config));
  const auto target_lock = target.acquire_lifecycle_lock();
  return convert_revisions(target.recover_revisions(options.version), options.version);
}

} // namespace dbdiff
