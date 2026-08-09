#include "dbdiff/error.hpp"
#include "dbdiff/lifecycle.hpp"
#include "dbdiff/migration.hpp"
#include "dbdiff/sqlite.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace {

dbdiff::Runtime isolated_runtime(int& environment_calls) {
  return dbdiff::Runtime{
      .environment = [&environment_calls](std::string_view) -> std::optional<std::string> {
        ++environment_calls;
        return std::nullopt;
      },
      .stdin_reader = [] { return std::string{}; },
      .now = [] { return std::chrono::system_clock::time_point{}; },
  };
}

} // namespace

TEST_CASE("explicit alternative configurations keep independent migration streams",
          "[unit][CFG-004]") {
  dbdiff::test::TempDirectory directory;
  const auto first_config = directory.write("first/dbdiff.yaml", R"yaml(format: 1
backend: sqlite
sources: [schema.sql]
migrations: migrations
)yaml");
  const auto second_config = directory.write("second/dbdiff.yaml", R"yaml(format: 1
backend: sqlite
sources: [schema.sql]
migrations: migrations
)yaml");
  directory.write("first/schema.sql", "CREATE TABLE first_only(id INTEGER PRIMARY KEY);\n");
  directory.write("second/schema.sql", "CREATE TABLE second_only(id INTEGER PRIMARY KEY);\n");
  int environment_calls = 0;
  const auto runtime = isolated_runtime(environment_calls);

  const auto first = dbdiff::create_migration(
      dbdiff::CreateOptions{first_config, "initial", {dbdiff::Hazard::write_lock}}, runtime);
  const auto second = dbdiff::create_migration(
      dbdiff::CreateOptions{second_config, "initial", {dbdiff::Hazard::write_lock}}, runtime);

  REQUIRE(first.created);
  REQUIRE(second.created);
  CHECK(first.file.parent_path() == directory.path() / "first/migrations");
  CHECK(second.file.parent_path() == directory.path() / "second/migrations");
  const auto first_migration = dbdiff::load_migration(first.file, dbdiff::BackendKind::sqlite);
  const auto second_migration = dbdiff::load_migration(second.file, dbdiff::BackendKind::sqlite);
  CHECK(first_migration.sql.find("first_only") != std::string::npos);
  CHECK(first_migration.sql.find("second_only") == std::string::npos);
  CHECK(second_migration.sql.find("second_only") != std::string::npos);
  CHECK(second_migration.sql.find("first_only") == std::string::npos);
  CHECK(environment_calls == 0);
}

TEST_CASE("draft migrations retain editable SQL but block reconstruction and apply",
          "[unit][CRE-005][PLN-004]") {
  dbdiff::test::TempDirectory directory;
  const auto config = directory.write("dbdiff.yaml", R"yaml(format: 1
backend: sqlite
database_env: DRAFT_TARGET
sources: [schema.sql]
migrations: migrations
)yaml");
  directory.write("schema.sql", "CREATE TABLE records(id INTEGER PRIMARY KEY);\n");
  int environment_calls = 0;
  auto now = std::chrono::system_clock::time_point{};
  const dbdiff::Runtime runtime{
      .environment = [&](const std::string_view name) -> std::optional<std::string> {
        ++environment_calls;
        if (name == "DRAFT_TARGET") {
          return "sqlite:" + (directory.path() / "target.sqlite").string();
        }
        return std::nullopt;
      },
      .stdin_reader = [] { return std::string{}; },
      .now =
          [&] {
            const auto result = now;
            now += std::chrono::seconds{1};
            return result;
          },
  };

  static_cast<void>(dbdiff::create_migration(
      dbdiff::CreateOptions{config, "initial", {dbdiff::Hazard::write_lock}}, runtime));
  directory.write("schema.sql",
                  "CREATE TABLE records(id INTEGER PRIMARY KEY, payload TEXT NOT NULL);\n");
  const auto draft = dbdiff::create_migration(
      dbdiff::CreateOptions{config,
                            "required payload",
                            {dbdiff::Hazard::table_rewrite, dbdiff::Hazard::constraint_scan,
                             dbdiff::Hazard::write_lock}},
      runtime);

  REQUIRE(draft.created);
  CHECK(draft.draft);
  const auto saved = dbdiff::load_migration(draft.file, dbdiff::BackendKind::sqlite);
  CHECK(saved.metadata.draft);
  CHECK(saved.sql.find("-- dbdiff:draft") != std::string::npos);
  CHECK(environment_calls == 0);

  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, true, false, false}, runtime),
      dbdiff::Error);
  CHECK(environment_calls == 0);
  CHECK_FALSE(std::filesystem::exists(directory.path() / "target.sqlite"));
}
