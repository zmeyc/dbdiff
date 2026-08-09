#pragma once

#include "dbdiff/backend.hpp"
#include "dbdiff/hazard.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff {

struct MigrationMetadata {
  int format{1};
  BackendKind backend;
  std::string version;
  std::string engine_version;
  std::string from_sha256;
  std::string to_sha256;
  std::string source_set_sha256;
  HazardSet allowed_hazards;
  bool draft{false};
};

struct MigrationFile {
  std::filesystem::path path;
  MigrationMetadata metadata;
  std::string sql;
  std::string exact_sha256;
};

[[nodiscard]] MigrationMetadata parse_migration_metadata(std::string_view sql);
[[nodiscard]] std::string render_migration_metadata(const MigrationMetadata& metadata);
void validate_engine_version(BackendKind backend, std::string_view version);
[[nodiscard]] std::string validate_migration_filename(const std::filesystem::path& file);
[[nodiscard]] MigrationFile load_migration(const std::filesystem::path& file,
                                           BackendKind expected_backend);
[[nodiscard]] std::vector<MigrationFile> load_migrations(const std::filesystem::path& directory,
                                                         BackendKind expected_backend);
void save_migration_atomic(const std::filesystem::path& directory, std::string_view version,
                           std::string_view sql);

} // namespace dbdiff
