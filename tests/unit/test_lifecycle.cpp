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

namespace {

dbdiff::Runtime fixed_runtime(int& environment_calls) {
  return dbdiff::Runtime{
      .environment = [&environment_calls](std::string_view) -> std::optional<std::string> {
        ++environment_calls;
        throw dbdiff::Error{dbdiff::ErrorCode::configuration,
                            "the live locator must not be resolved"};
      },
      .stdin_reader = [] { return std::string{}; },
      .now = [] { return std::chrono::system_clock::time_point{}; },
  };
}

std::filesystem::path write_sqlite_project(dbdiff::test::TempDirectory& directory) {
  directory.write("schema/00_utils.sql", R"sql(
CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
)sql");
  directory.write("schema/10_auth.sql", R"sql(
CREATE TABLE users(
  id INTEGER PRIMARY KEY,
  email TEXT NOT NULL UNIQUE
);
)sql");
  return directory.write("dbdiff.yaml", R"yaml(format: 1
backend: sqlite
database_env: LIVE_DATABASE_MUST_NOT_BE_RESOLVED
sources:
  - schema
migrations: migrations
)yaml");
}

} // namespace

TEST_CASE("migration versions are UTC and names are normalized", "[unit][CRE-003]") {
  CHECK(dbdiff::make_migration_version(std::chrono::system_clock::time_point{},
                                       "Create Auth users!") == "19700101000000_create_auth_users");
  CHECK_THROWS_AS(dbdiff::make_migration_version(std::chrono::system_clock::time_point{}, "---"),
                  dbdiff::Error);
}

TEST_CASE("create reconstructs fresh databases without resolving the live target",
          "[unit][CRE-001][CRE-002][CRE-003][CRE-004][MIG-005]") {
  dbdiff::test::TempDirectory directory;
  const auto config = write_sqlite_project(directory);
  int environment_calls = 0;
  const auto runtime = fixed_runtime(environment_calls);

  const auto result = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "initial auth", {dbdiff::Hazard::write_lock}}, runtime);

  REQUIRE(result.created);
  CHECK_FALSE(result.draft);
  CHECK(result.version == "19700101000000_initial_auth");
  CHECK(result.file == directory.path() / "migrations/19700101000000_initial_auth.sql");
  CHECK(environment_calls == 0);
  CHECK_FALSE(std::filesystem::exists(directory.path() / "live.sqlite"));

  const auto migration = dbdiff::load_migration(result.file, dbdiff::BackendKind::sqlite);
  CHECK(migration.metadata.from_sha256 != migration.metadata.to_sha256);
  CHECK(migration.metadata.allowed_hazards == dbdiff::HazardSet{dbdiff::Hazard::write_lock});
  CHECK(migration.sql.find("BEGIN IMMEDIATE;") != std::string::npos);
  CHECK(migration.sql.find("COMMIT;") != std::string::npos);

  const auto unchanged = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "must not be saved", {dbdiff::Hazard::write_lock}}, runtime);
  CHECK_FALSE(unchanged.created);
  CHECK(environment_calls == 0);
}

TEST_CASE("create requires every generated hazard before saving", "[unit][PLN-003]") {
  dbdiff::test::TempDirectory directory;
  const auto config = write_sqlite_project(directory);
  int environment_calls = 0;
  const auto runtime = fixed_runtime(environment_calls);

  CHECK_THROWS_AS(dbdiff::create_migration(dbdiff::CreateOptions{config, "initial", {}}, runtime),
                  dbdiff::Error);
  CHECK_FALSE(std::filesystem::exists(directory.path() / "migrations"));
  CHECK(environment_calls == 0);
}

TEST_CASE("SQLite apply, status, drift, and recover use only database-recorded history",
          "[unit][APP-001][APP-002][APP-003][APP-005][APP-006][MIG-004]") {
  dbdiff::test::TempDirectory directory;
  const auto config = write_sqlite_project(directory);
  const auto database_path = directory.path() / "live.sqlite";
  int environment_calls = 0;
  const dbdiff::Runtime runtime{
      .environment = [&](const std::string_view name) -> std::optional<std::string> {
        ++environment_calls;
        if (name != "LIVE_DATABASE_MUST_NOT_BE_RESOLVED") {
          return std::nullopt;
        }
        return "sqlite:" + database_path.string();
      },
      .stdin_reader = [] { return std::string{}; },
      .now = [] { return std::chrono::system_clock::time_point{}; },
  };

  const auto created = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "initial", {dbdiff::Hazard::write_lock}}, runtime);
  REQUIRE(created.created);
  CHECK(environment_calls == 0);

  const auto dry_run =
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, true, true, false, false}, runtime);
  CHECK(dry_run.pending == 1U);
  CHECK(dry_run.applied == 0U);
  CHECK(dry_run.dry_run);
  CHECK_FALSE(std::filesystem::exists(database_path));

  const auto applied =
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, true, false, false}, runtime);
  CHECK(applied.pending == 1U);
  CHECK(applied.applied == 1U);
  CHECK(std::filesystem::is_regular_file(database_path));
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::converged);

  const auto revisions =
      dbdiff::recover_migration(dbdiff::RecoverOptions{config, created.version, false}, runtime);
  REQUIRE(revisions.size() == 1U);
  CHECK(revisions[0].ordinal == 0U);
  CHECK(revisions[0].sql == dbdiff::load_migration(created.file, dbdiff::BackendKind::sqlite).sql);

  {
    auto target =
        dbdiff::sqlite::Database::open(database_path, dbdiff::sqlite::OpenMode::read_write);
    target.execute_migration("BEGIN; CREATE TABLE manual_drift(id INTEGER); COMMIT;");
  }
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::drift);
  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, false}, runtime),
      dbdiff::Error);
}

TEST_CASE("create allocates monotonically ordered versions within one clock second",
          "[unit][MIG-001][CRE-003]") {
  dbdiff::test::TempDirectory directory;
  const auto config = write_sqlite_project(directory);
  int environment_calls = 0;
  const auto runtime = fixed_runtime(environment_calls);
  const auto first = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "initial", {dbdiff::Hazard::write_lock}}, runtime);
  REQUIRE(first.version == "19700101000000_initial");

  directory.write("schema/20_audit.sql", "CREATE TABLE audit(id INTEGER PRIMARY KEY);\n");
  const auto second = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "add audit", {dbdiff::Hazard::write_lock}}, runtime);
  CHECK(second.version == "19700101000001_add_audit");
  const auto migrations =
      dbdiff::load_migrations(directory.path() / "migrations", dbdiff::BackendKind::sqlite);
  REQUIRE(migrations.size() == 2U);
  CHECK(migrations[0].metadata.version == first.version);
  CHECK(migrations[1].metadata.version == second.version);
}

TEST_CASE("SQLite data validation uses an online copy and never mutates the target on failure",
          "[unit][APP-005][SQT-003][SQT-006]") {
  dbdiff::test::TempDirectory directory;
  const auto database_path = directory.path() / "data.sqlite";
  const auto config = directory.write("dbdiff.yaml", R"yaml(format: 1
backend: sqlite
database: sqlite:data.sqlite
sources: [schema.sql]
migrations: migrations
)yaml");
  directory.write("schema.sql", R"sql(
CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT NOT NULL) STRICT;
)sql");
  auto now = std::chrono::system_clock::time_point{};
  const dbdiff::Runtime runtime{
      .environment = [](std::string_view) { return std::optional<std::string>{}; },
      .stdin_reader = [] { return std::string{}; },
      .now =
          [&] {
            const auto result = now;
            now += std::chrono::seconds{1};
            return result;
          },
  };

  const auto initial = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "initial", {dbdiff::Hazard::write_lock}}, runtime);
  REQUIRE(initial.created);
  static_cast<void>(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, true, false, false}, runtime));
  {
    auto target =
        dbdiff::sqlite::Database::open(database_path, dbdiff::sqlite::OpenMode::read_write);
    target.execute_migration(
        "BEGIN IMMEDIATE; INSERT INTO items(id,value) VALUES(1,'bad'); COMMIT;");
  }

  directory.write("schema.sql", R"sql(
CREATE TABLE items(
  id INTEGER PRIMARY KEY,
  value TEXT NOT NULL CHECK(value = 'good')
) STRICT;
)sql");
  const auto constrained = dbdiff::create_migration(
      dbdiff::CreateOptions{config,
                            "constrain values",
                            {dbdiff::Hazard::table_rewrite, dbdiff::Hazard::constraint_scan,
                             dbdiff::Hazard::write_lock}},
      runtime);
  REQUIRE(constrained.created);

  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, true, false}, runtime),
      dbdiff::Error);
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::pending);

  {
    auto target =
        dbdiff::sqlite::Database::open(database_path, dbdiff::sqlite::OpenMode::read_write);
    target.execute_migration("BEGIN IMMEDIATE; UPDATE items SET value='good'; COMMIT;");
  }
  const auto applied =
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, true, false}, runtime);
  CHECK(applied.applied == 1U);
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::converged);
}

TEST_CASE("SQLite lifecycle resumes only an edited incomplete suffix",
          "[unit][MIG-002][MIG-003][MIG-004][SQL-003][APP-006]") {
  dbdiff::test::TempDirectory directory;
  const auto config = write_sqlite_project(directory);
  const auto database_path = directory.path() / "resume.sqlite";
  auto now = std::chrono::system_clock::time_point{};
  const dbdiff::Runtime runtime{
      .environment = [&](const std::string_view name) -> std::optional<std::string> {
        if (name == "LIVE_DATABASE_MUST_NOT_BE_RESOLVED") {
          return "sqlite:" + database_path.string();
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
  static_cast<void>(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, true, false, false}, runtime));
  {
    auto target =
        dbdiff::sqlite::Database::open(database_path, dbdiff::sqlite::OpenMode::read_write);
    target.execute_migration(
        "BEGIN IMMEDIATE; INSERT INTO users(email) VALUES('taken@example.test'); COMMIT;");
  }

  directory.write("schema/20_audit.sql",
                  "CREATE TABLE audit_log(id INTEGER PRIMARY KEY, message TEXT);\n");
  const auto second = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "audit", {dbdiff::Hazard::write_lock}}, runtime);
  REQUIRE(second.created);
  const auto generated = dbdiff::load_migration(second.file, dbdiff::BackendKind::sqlite).sql;
  const auto failed = generated + R"sql(
BEGIN IMMEDIATE;
INSERT INTO users(email) VALUES('taken@example.test');
COMMIT;
)sql";
  directory.write("migrations/" + second.version + ".sql", failed);

  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, false}, runtime),
      dbdiff::Error);
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::pending);

  auto changed_prefix = failed;
  const auto create_position = changed_prefix.find("CREATE TABLE audit_log");
  REQUIRE(create_position != std::string::npos);
  changed_prefix.replace(create_position, std::string{"CREATE TABLE"}.size(), "CREATE  TABLE");
  directory.write("migrations/" + second.version + ".sql", changed_prefix);
  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, true}, runtime),
      dbdiff::Error);

  auto repaired = failed;
  const auto value_position = repaired.rfind("taken@example.test");
  REQUIRE(value_position != std::string::npos);
  repaired.replace(value_position, std::string{"taken@example.test"}.size(),
                   "recovered@example.test");
  directory.write("migrations/" + second.version + ".sql", repaired);
  const auto resumed =
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, true}, runtime);
  CHECK(resumed.applied == 1U);
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::converged);

  const auto revisions =
      dbdiff::recover_migration(dbdiff::RecoverOptions{config, second.version, true}, runtime);
  REQUIRE(revisions.size() == 2U);
  CHECK(revisions[0].sql == failed);
  CHECK(revisions[1].sql == repaired);
}
