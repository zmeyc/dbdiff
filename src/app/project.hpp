#pragma once

#include "dbdiff/docker.hpp"
#include "dbdiff/error.hpp"
#include "dbdiff/lifecycle.hpp"
#include "dbdiff/migration.hpp"
#include "dbdiff/postgresql.hpp"
#include "dbdiff/sqlite.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff::app_detail {

struct ProjectInputs {
  Config config;
  SourceSet sources;
  std::vector<MigrationFile> migrations;
};

[[noreturn]] void lifecycle_error(ErrorCode code, const std::string& message);

class ProjectLifecycleLock final {
public:
  ProjectLifecycleLock(const std::filesystem::path& config_file, std::chrono::milliseconds timeout);
  ProjectLifecycleLock(const ProjectLifecycleLock&) = delete;
  ProjectLifecycleLock& operator=(const ProjectLifecycleLock&) = delete;
  ProjectLifecycleLock(ProjectLifecycleLock&&) = delete;
  ProjectLifecycleLock& operator=(ProjectLifecycleLock&&) = delete;
  ~ProjectLifecycleLock();

private:
  void close_descriptor() noexcept;
  int descriptor_{-1};
};

sqlite::ConnectionSettings sqlite_settings(const Config& config);
postgresql::ConnectionSettings postgresql_settings(const Config& config);
ProjectInputs load_inputs(Config config, const Runtime& runtime);
ProjectInputs load_history_inputs(Config config);
std::string require_target_locator(const Config& config, const Runtime& runtime);
std::filesystem::path sqlite_target_path(const Config& config, const Runtime& runtime);
bool sqlite_target_exists(const std::filesystem::path& path);
std::string concatenate_sources(const SourceSet& sources);

struct PostgresProvisioning {
  std::string locator;
  std::optional<docker::PostgresContainer> container;
};
PostgresProvisioning provision_postgresql(const Config& config, const Runtime& runtime);

struct AppliedPrefix {
  std::size_t completed_versions{0};
  std::optional<std::size_t> incomplete_unit_count;
  bool operator==(const AppliedPrefix&) const = default;
};

AppliedPrefix validate_sqlite_history(const sqlite::MigrationHistory& history,
                                      const std::vector<MigrationFile>& migrations);
AppliedPrefix validate_postgresql_history(const postgresql::MigrationHistory& history,
                                          const std::vector<MigrationFile>& migrations);
void require_migration_start(const MigrationFile& migration, std::string_view before);
void require_migration_end(const MigrationFile& migration, std::string_view after);
sqlite::SchemaSnapshot replay_sqlite(const std::vector<MigrationFile>& migrations,
                                     sqlite::Database& database);
postgresql::SchemaSnapshot replay_postgresql(const std::vector<MigrationFile>& migrations,
                                             postgresql::ScratchDatabase& database,
                                             const std::vector<std::string>& managed_schemas);

struct SqlitePrefixReconstruction {
  AppliedPrefix prefix;
  sqlite::SchemaSnapshot snapshot;
};
SqlitePrefixReconstruction reconstruct_sqlite_prefix(const sqlite::MigrationHistory& history,
                                                     const std::vector<MigrationFile>& migrations,
                                                     sqlite::Database& database,
                                                     std::string_view live_schema_sha256);

struct PostgresPrefixReconstruction {
  AppliedPrefix prefix;
  postgresql::SchemaSnapshot snapshot;
};
PostgresPrefixReconstruction reconstruct_postgresql_prefix(
    const postgresql::MigrationHistory& history, const std::vector<MigrationFile>& migrations,
    postgresql::ScratchDatabase& scratch, const std::vector<std::string>& managed_schemas);

} // namespace dbdiff::app_detail
