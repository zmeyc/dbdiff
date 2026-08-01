#include "dbdiff/config.hpp"
#include "dbdiff/error.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

using namespace std::chrono_literals;

TEST_CASE("strict YAML configuration resolves paths and defaults", "[unit][CFG-001]") {
  dbdiff::test::TempDirectory temporary;
  const auto file = temporary.write("project/dbdiff.yaml",
                                    R"(format: 1
backend: postgresql
database_env: APP_DATABASE_URL
sources:
  - schema
  - shared.sql
migrations: migrations/postgresql
scratch:
  docker:
    image: postgres:18
lock_timeout: 7s
statement_timeout: 2m
)");

  const auto config = dbdiff::load_config(file);
  CHECK(config.file == std::filesystem::absolute(file).lexically_normal());
  CHECK(config.backend == dbdiff::BackendKind::postgresql);
  CHECK(config.database.environment_variable == "APP_DATABASE_URL");
  REQUIRE(config.sources.size() == 2);
  CHECK(config.sources[0] == file.parent_path() / "schema");
  CHECK(config.migrations == file.parent_path() / "migrations/postgresql");
  REQUIRE(config.scratch.docker);
  CHECK(config.scratch.docker.value_or(dbdiff::DockerScratchConfig{}).image == "postgres:18");
  CHECK(config.managed_schemas == std::vector<std::string>{"public"});
  CHECK(config.lock_timeout == 7s);
  CHECK(config.statement_timeout == 2min);
}

TEST_CASE("SQLite configuration rejects PostgreSQL-only fields", "[unit][CFG-001]") {
  dbdiff::test::TempDirectory temporary;
  const auto file = temporary.write("dbdiff.yaml",
                                    R"(format: 1
backend: sqlite
sources: [schema.sql]
migrations: migrations
managed_schemas: [public]
)");
  CHECK_THROWS_AS(dbdiff::load_config(file), dbdiff::Error);
}

TEST_CASE("configuration rejects unknown, ambiguous, and advanced YAML", "[unit][CFG-001]") {
  dbdiff::test::TempDirectory temporary;

  SECTION("unknown key") {
    const auto file = temporary.write(
        "unknown.yaml",
        "format: 1\nbackend: sqlite\nsources: [a.sql]\nmigrations: m\nextra: true\n");
    CHECK_THROWS_AS(dbdiff::load_config(file), dbdiff::Error);
  }
  SECTION("invalid format type") {
    const auto file = temporary.write(
        "format.yaml", "format: current\nbackend: sqlite\nsources: [a.sql]\nmigrations: m\n");
    CHECK_THROWS_AS(dbdiff::load_config(file), dbdiff::Error);
  }
  SECTION("two locators") {
    const auto file = temporary.write(
        "locator.yaml", "format: 1\nbackend: sqlite\ndatabase: sqlite:a.db\ndatabase_env: DB\n"
                        "sources: [a.sql]\nmigrations: m\n");
    CHECK_THROWS_AS(dbdiff::load_config(file), dbdiff::Error);
  }
  SECTION("alias") {
    const auto file =
        temporary.write("alias.yaml", "format: 1\nbackend: sqlite\nsources: &sources [a.sql]\n"
                                      "migrations: m\ncopy: *sources\n");
    CHECK_THROWS_AS(dbdiff::load_config(file), dbdiff::Error);
  }
  SECTION("invalid duration") {
    const auto file = temporary.write(
        "duration.yaml",
        "format: 1\nbackend: sqlite\nsources: [a.sql]\nmigrations: m\nlock_timeout: 5\n");
    CHECK_THROWS_AS(dbdiff::load_config(file), dbdiff::Error);
  }
}

TEST_CASE("configuration discovery stops at the nearest Git root", "[unit][CFG-001]") {
  dbdiff::test::TempDirectory temporary;
  std::filesystem::create_directories(temporary.path() / "repo/.git");
  std::filesystem::create_directories(temporary.path() / "repo/a/b");
  temporary.write("repo/dbdiff.yaml", "root");
  temporary.write("repo/a/dbdiff.yaml", "nearest");
  temporary.write("dbdiff.yaml", "outside");

  CHECK(dbdiff::discover_config(temporary.path() / "repo/a/b") ==
        temporary.path() / "repo/a/dbdiff.yaml");
  std::filesystem::remove(temporary.path() / "repo/a/dbdiff.yaml");
  CHECK(dbdiff::discover_config(temporary.path() / "repo/a/b") ==
        temporary.path() / "repo/dbdiff.yaml");
}

TEST_CASE("locator inference, environment resolution, and redaction are deterministic",
          "[unit][CFG-002]") {
  CHECK(dbdiff::infer_backend("postgresql://localhost/db") == dbdiff::BackendKind::postgresql);
  CHECK(dbdiff::infer_backend("POSTGRES://localhost/db") == dbdiff::BackendKind::postgresql);
  CHECK(dbdiff::infer_backend("sqlite:relative.db") == dbdiff::BackendKind::sqlite);
  CHECK_FALSE(dbdiff::infer_backend("host=localhost dbname=app"));

  const dbdiff::LocatorConfig from_environment{std::nullopt, "APP_URL"};
  const auto resolved = dbdiff::resolve_locator(
      from_environment, [](const std::string_view name) -> std::optional<std::string> {
        return name == "APP_URL" ? std::optional<std::string>{"postgresql://db"} : std::nullopt;
      });
  CHECK(resolved == "postgresql://db");
  CHECK_THROWS_AS(
      dbdiff::resolve_locator(from_environment,
                              [](std::string_view) { return std::optional<std::string>{}; }),
      dbdiff::Error);

  CHECK(dbdiff::redact_locator("postgresql://alice:secret@localhost/app") ==
        "postgresql://alice:***@localhost/app");
  CHECK(dbdiff::redact_locator("host=localhost password='very secret' user=alice") ==
        "host=localhost password=*** user=alice");
  CHECK(dbdiff::redact_locator("sqlite:db/app.sqlite") == "sqlite:db/app.sqlite");
}
