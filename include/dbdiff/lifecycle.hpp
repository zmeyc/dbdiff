#pragma once

#include "dbdiff/config.hpp"
#include "dbdiff/hazard.hpp"
#include "dbdiff/operation.hpp"
#include "dbdiff/source.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff {

struct Runtime {
  EnvironmentLookup environment;
  StdinReader stdin_reader;
  std::function<std::chrono::system_clock::time_point()> now;
};

[[nodiscard]] Runtime default_runtime();

struct CreateOptions {
  std::filesystem::path config_file;
  std::string name;
  HazardSet allowed_hazards;
};

struct CreateResult {
  bool created{false};
  bool draft{false};
  std::filesystem::path file;
  std::string version;
  HazardSet hazards;
};

struct ApplyOptions {
  std::filesystem::path config_file;
  bool dry_run{false};
  bool create_database{false};
  bool validate_data{false};
  bool resume{false};
};

struct ApplyResult {
  std::size_t pending{0};
  std::size_t applied{0};
  bool dry_run{false};
};

enum class ProjectStatus { converged, pending, drift, missing_database };

struct StatusResult {
  ProjectStatus status{ProjectStatus::converged};
  std::size_t applied{0};
  std::size_t total{0};
  std::string detail;
};

struct RecoverOptions {
  std::filesystem::path config_file;
  std::string version;
  bool list{false};
};

struct RecoveredRevision {
  std::size_t ordinal{0};
  std::string exact_sha256;
  std::string sql;
};

[[nodiscard]] CreateResult create_migration(const CreateOptions& options,
                                            const Runtime& runtime = default_runtime());
[[nodiscard]] ApplyResult apply_migrations(const ApplyOptions& options,
                                           const Runtime& runtime = default_runtime());
[[nodiscard]] StatusResult project_status(const std::filesystem::path& config_file,
                                          const Runtime& runtime = default_runtime());
[[nodiscard]] std::vector<RecoveredRevision>
recover_migration(const RecoverOptions& options, const Runtime& runtime = default_runtime());

[[nodiscard]] std::string make_migration_version(std::chrono::system_clock::time_point time,
                                                 std::string_view name);
[[nodiscard]] HazardSet collect_hazards(const std::vector<Operation>& operations);
void require_hazard_approvals(const HazardSet& required, const HazardSet& allowed);

} // namespace dbdiff
