#include "dbdiff/cli.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hazard.hpp"
#include "dbdiff/version.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff {
namespace {

std::filesystem::path resolve_config_file(const std::string& explicit_file,
                                          const std::filesystem::path& working_directory) {
  if (!explicit_file.empty()) {
    const std::filesystem::path path{explicit_file};
    return path.is_absolute() ? path : working_directory / path;
  }
  const auto discovered = discover_config(working_directory);
  if (!discovered) {
    throw Error{ErrorCode::configuration,
                "no dbdiff.yaml found before the nearest Git root; use --config"};
  }
  return *discovered;
}

HazardSet parse_hazards(const std::vector<std::string>& names) {
  HazardSet hazards;
  for (const auto& name : names) {
    const auto hazard = parse_hazard(name);
    if (!hazard) {
      throw Error{ErrorCode::configuration, "unknown hazard '" + name + "'"};
    }
    if (!hazards.insert(*hazard).second) {
      throw Error{ErrorCode::configuration, "hazard '" + name + "' was specified more than once"};
    }
  }
  return hazards;
}

} // namespace

int run_cli(const int argc, char** argv, std::ostream& output, std::ostream& error,
            const std::filesystem::path& working_directory, const Runtime& runtime) {
  CLI::App app{"Reconstruct, diff, and apply deterministic database schemas", "dbdiff"};
  app.set_version_flag("--version", std::string{version()});
  app.require_subcommand(1);

  std::string config_file;
  app.add_option("-c,--config", config_file, "Configuration file (default: discover dbdiff.yaml)");

  std::string migration_name;
  std::vector<std::string> allowed_hazard_names;
  auto* create =
      app.add_subcommand("create", "Reconstruct history and master, then save a migration");
  create->add_option("--name", migration_name, "Migration name")->required();
  create->add_option("--allow-hazard", allowed_hazard_names,
                     "Approve a typed hazard in the saved SQL metadata");

  bool dry_run = false;
  bool create_database = false;
  bool validate_data = false;
  bool resume = false;
  auto* apply = app.add_subcommand("apply", "Validate drift and apply pending migrations");
  apply->add_flag("--dry-run", dry_run, "Validate without writing the target");
  apply->add_flag("--create-database", create_database,
                  "Create an absent SQLite target explicitly");
  apply->add_flag("--validate-data", validate_data,
                  "Validate a copy of live SQLite data before writing");
  apply->add_flag("--resume", resume, "Resume an incomplete migration revision");

  auto* status = app.add_subcommand("status", "Report convergence, pending work, or drift");

  std::string recover_version;
  bool list_revisions = false;
  auto* recover =
      app.add_subcommand("recover", "Read stored migration SQL revisions from the database");
  recover->add_option("version", recover_version, "Migration version")->required();
  recover->add_flag("--list", list_revisions,
                    "List stored revision hashes instead of printing SQL");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& parse_error) {
    return app.exit(parse_error, output, error);
  }

  try {
    const auto config = resolve_config_file(config_file, working_directory);
    if (*create) {
      const auto result = create_migration(
          CreateOptions{config, migration_name, parse_hazards(allowed_hazard_names)}, runtime);
      if (!result.created) {
        output << "Schema is unchanged; no migration created.\n";
      } else {
        output << (result.draft ? "Saved draft migration " : "Saved migration ")
               << result.file.string() << '\n';
      }
      return 0;
    }
    if (*apply) {
      const auto result = apply_migrations(
          ApplyOptions{config, dry_run, create_database, validate_data, resume}, runtime);
      if (result.dry_run) {
        output << "Validated " << result.pending << " pending migration(s); no writes performed.\n";
      } else {
        output << "Applied " << result.applied << " migration(s).\n";
      }
      return 0;
    }
    if (*status) {
      const auto result = project_status(config, runtime);
      output << result.detail << '\n';
      return result.status == ProjectStatus::converged ? 0 : 2;
    }
    if (*recover) {
      const auto revisions =
          recover_migration(RecoverOptions{config, recover_version, list_revisions}, runtime);
      if (list_revisions) {
        for (const auto& revision : revisions) {
          output << recover_version << '\t' << revision.ordinal << '\t' << revision.exact_sha256
                 << '\n';
        }
      } else {
        const auto& revision = revisions.back();
        output << revision.sql;
        if (!revision.sql.ends_with('\n')) {
          output << '\n';
        }
      }
      return 0;
    }
  } catch (const Error& command_error) {
    error << "dbdiff: " << command_error.what() << '\n';
    return 1;
  } catch (const std::exception& command_error) {
    error << "dbdiff: unexpected failure: " << command_error.what() << '\n';
    return 1;
  }
  return 1;
}

} // namespace dbdiff
