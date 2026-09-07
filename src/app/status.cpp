#include "dbdiff/lifecycle.hpp"

#include "project.hpp"

#include <string>
#include <utility>

namespace dbdiff {
namespace {

using namespace app_detail;

StatusResult history_status(const AppliedPrefix& prefix, const std::size_t total,
                            const std::string& backend, const bool drift_checked) {
  const bool pending =
      prefix.completed_versions != total || prefix.incomplete_unit_count.has_value();
  if (!drift_checked) {
    return StatusResult{
        pending ? ProjectStatus::pending : ProjectStatus::history_up_to_date,
        prefix.completed_versions, total,
        backend + (pending ? " migrations are pending" : " migration history is up to date") +
            "; schema drift was not checked",
        false};
  }
  return StatusResult{pending ? ProjectStatus::pending : ProjectStatus::converged,
                      prefix.completed_versions, total,
                      backend + " schema matches its applied migrations; " +
                          (pending ? "migrations are pending" : "no pending migrations"),
                      true};
}

StatusResult status_sqlite(ProjectInputs& project, const StatusOptions& options,
                           const Runtime& runtime) {
  const auto path = sqlite_target_path(project.config, runtime);
  if (!sqlite_target_exists(path)) {
    return StatusResult{ProjectStatus::missing_database, 0, project.migrations.size(),
                        "SQLite database is missing; schema drift was not checked", false};
  }

  auto target =
      sqlite::Database::open(path, sqlite::OpenMode::read_only, sqlite_settings(project.config));
  const auto history = target.read_history();
  if (options.quick) {
    return history_status(validate_sqlite_history(history, project.migrations),
                          project.migrations.size(), "SQLite", false);
  }

  const auto live = target.inspect();
  auto prefix_database = sqlite::Database::temporary(sqlite_settings(project.config));
  const auto prefix =
      reconstruct_sqlite_prefix(history, project.migrations, prefix_database, live.semantic_hash);
  if (live.semantic_hash != prefix.snapshot.semantic_hash) {
    return StatusResult{ProjectStatus::drift, prefix.prefix.completed_versions,
                        project.migrations.size(),
                        "Live SQLite schema has drifted from its recorded migration prefix", true};
  }
  return history_status(prefix.prefix, project.migrations.size(), "SQLite", true);
}

StatusResult status_postgresql(ProjectInputs& project, const StatusOptions& options,
                               const Runtime& runtime) {
  auto target = postgresql::Database::open(require_target_locator(project.config, runtime),
                                           postgresql_settings(project.config));
  const auto lifecycle_lock = target.acquire_lifecycle_lock();
  const auto history = target.read_history();
  if (options.quick) {
    return history_status(validate_postgresql_history(history, project.migrations),
                          project.migrations.size(), "PostgreSQL", false);
  }

  auto provisioning = provision_postgresql(project.config, runtime);
  auto prefix_database = postgresql::ScratchDatabase::create(provisioning.locator,
                                                             postgresql_settings(project.config));
  const auto prefix = reconstruct_postgresql_prefix(history, project.migrations, prefix_database,
                                                    project.config.managed_schemas);
  if (target.server_version().major != prefix.snapshot.server_version.major) {
    lifecycle_error(ErrorCode::unsupported,
                    "target and scratch PostgreSQL major versions must match");
  }
  if (target.introspect(project.config.managed_schemas).semantic_hash !=
      prefix.snapshot.semantic_hash) {
    return StatusResult{
        ProjectStatus::drift, prefix.prefix.completed_versions, project.migrations.size(),
        "Live PostgreSQL schema has drifted from its recorded migration prefix", true};
  }
  return history_status(prefix.prefix, project.migrations.size(), "PostgreSQL", true);
}

} // namespace

StatusResult project_status(const StatusOptions& options, const Runtime& runtime) {
  auto config = load_config(options.config_file);
  const app_detail::ProjectLifecycleLock lifecycle_lock{config.file, config.lock_timeout};
  auto project = app_detail::load_history_inputs(std::move(config));
  if (project.config.backend == BackendKind::sqlite) {
    return status_sqlite(project, options, runtime);
  }
  return status_postgresql(project, options, runtime);
}

StatusResult project_status(const std::filesystem::path& config_file, const Runtime& runtime) {
  return project_status(StatusOptions{config_file, false}, runtime);
}

} // namespace dbdiff
