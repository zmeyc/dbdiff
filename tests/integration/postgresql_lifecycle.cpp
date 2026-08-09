#include "dbdiff/docker.hpp"
#include "dbdiff/error.hpp"
#include "dbdiff/hazard.hpp"
#include "dbdiff/lifecycle.hpp"
#include "dbdiff/migration.hpp"
#include "dbdiff/postgresql.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
namespace docker = dbdiff::docker;
namespace postgresql = dbdiff::postgresql;

namespace {

int selected_major() {
  const char* value = std::getenv("DBDIFF_TEST_POSTGRES_MAJOR"); // NOLINT(concurrency-mt-unsafe)
  if (value == nullptr || *value == '\0') {
    return 18;
  }
  const std::string_view text{value};
  int major = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), major);
  if (error != std::errc{} || end != text.data() + text.size() || major < 15 || major > 18) {
    FAIL("DBDIFF_TEST_POSTGRES_MAJOR must be 15, 16, 17, or 18");
  }
  return major;
}

std::string trim_ascii(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

std::string scratch_database_count(const docker::PostgresContainer& container,
                                   const std::shared_ptr<docker::ProcessRunner>& runner) {
  constexpr std::string_view statement = "SELECT count(*) FROM pg_catalog.pg_database "
                                         "WHERE datname ~ '^dbdiff_[0-9a-f]{32}$';";
  const auto query =
      runner->run({"docker", "exec", std::string{container.container_id()}, "psql", "--username",
                   std::string{container.username()}, "--dbname", std::string{container.database()},
                   "--tuples-only", "--no-align", "--command", std::string{statement}},
                  30s);
  REQUIRE(query.succeeded());
  return trim_ascii(query.standard_output);
}

std::string postgres_config(const std::string_view scratch_locator) {
  return "format: 1\n"
         "backend: postgresql\n"
         "database_env: DBDIFF_INTEGRATION_TARGET\n"
         "sources:\n"
         "  - schema\n"
         "migrations: migrations\n"
         "scratch:\n"
         "  database: '" +
         std::string{scratch_locator} +
         "'\n"
         "managed_schemas:\n"
         "  - public\n"
         "  - app\n"
         "lock_timeout: 5s\n"
         "statement_timeout: 1m\n";
}

} // namespace

TEST_CASE("PostgreSQL lifecycle is isolated, dry-run safe, recoverable, and drift gated",
          "[integration][docker][postgresql][lifecycle][CRE-001][APP-001][APP-002][APP-003]"
          "[APP-005][MIG-004][SCR-002][SCR-004]") {
  auto runner = docker::make_system_process_runner();
  const auto docker_status = runner->run({"docker", "info", "--format", "{{.ServerVersion}}"}, 10s);
  if (!docker_status.succeeded()) {
    SKIP("Docker CLI or daemon is unavailable");
  }

  docker::PostgresContainerOptions container_options;
  container_options.postgres_major = selected_major();
  container_options.command_timeout = 5min;
  container_options.readiness_timeout = 90s;
  if (const char* image =
          std::getenv("DBDIFF_TEST_POSTGRES_IMAGE"); // NOLINT(concurrency-mt-unsafe)
      image != nullptr && *image != '\0') {
    container_options.image = image;
  }

  auto container = docker::PostgresContainer::create(container_options, runner);
  INFO("PostgreSQL " << container_options.postgres_major << " target " << container.redacted_dsn());
  REQUIRE(scratch_database_count(container, runner) == "0");

  dbdiff::test::TempDirectory project;
  project.write("schema/00_core.sql", R"sql(
CREATE TABLE public.accounts(
  id bigint NOT NULL,
  email text NOT NULL
);
)sql");
  project.write("schema/10_auth.sql", R"sql(
ALTER TABLE public.accounts ENABLE ROW LEVEL SECURITY;
CREATE SCHEMA app;
CREATE TABLE app.sessions(
  account_id bigint NOT NULL,
  token text
);
)sql");
  const auto config = project.write("dbdiff.yaml", postgres_config(container.connection_dsn()));

  bool expose_target = false;
  std::size_t target_locator_lookups = 0U;
  const auto target_locator = container.connection_dsn();
  const dbdiff::Runtime runtime{
      .environment = [&](const std::string_view name) -> std::optional<std::string> {
        if (name != "DBDIFF_INTEGRATION_TARGET") {
          return std::nullopt;
        }
        ++target_locator_lookups;
        if (!expose_target) {
          return "postgresql://127.0.0.1:1/unreachable";
        }
        return target_locator;
      },
      .stdin_reader = [] { return std::string{}; },
      .now = [] { return std::chrono::system_clock::time_point{}; },
  };

  const auto created = dbdiff::create_migration(
      dbdiff::CreateOptions{config, "initial auth", {dbdiff::Hazard::write_lock}}, runtime);
  REQUIRE(created.created);
  CHECK_FALSE(created.draft);
  CHECK(target_locator_lookups == 0U);
  CHECK(scratch_database_count(container, runner) == "0");

  const auto migration = dbdiff::load_migration(created.file, dbdiff::BackendKind::postgresql);
  CHECK(migration.sql.find("BEGIN;") != std::string::npos);
  CHECK(migration.sql.find("COMMIT;") != std::string::npos);

  expose_target = true;
  {
    auto target = postgresql::Database::open(target_locator);
    const auto before = target.introspect({"public", "app"});
    CHECK_FALSE(target.read_history().initialized);

    const auto dry_run =
        dbdiff::apply_migrations(dbdiff::ApplyOptions{config, true, false, false, false}, runtime);
    CHECK(dry_run.pending == 1U);
    CHECK(dry_run.applied == 0U);
    CHECK(dry_run.dry_run);

    CHECK_FALSE(target.read_history().initialized);
    CHECK(target.introspect({"public", "app"}).semantic_hash == before.semantic_hash);
  }
  CHECK(scratch_database_count(container, runner) == "0");
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::pending);

  const auto applied =
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, false}, runtime);
  CHECK(applied.pending == 1U);
  CHECK(applied.applied == 1U);
  CHECK_FALSE(applied.dry_run);
  CHECK(scratch_database_count(container, runner) == "0");

  const auto status = dbdiff::project_status(config, runtime);
  CHECK(status.status == dbdiff::ProjectStatus::converged);
  CHECK(status.applied == 1U);
  CHECK(status.total == 1U);

  const auto revisions =
      dbdiff::recover_migration(dbdiff::RecoverOptions{config, created.version, false}, runtime);
  REQUIRE(revisions.size() == 1U);
  CHECK(revisions[0].ordinal == 0U);
  CHECK(revisions[0].exact_sha256 == migration.exact_sha256);
  CHECK(revisions[0].sql == migration.sql);

  {
    auto target = postgresql::Database::open(target_locator);
    const auto history = target.read_history();
    REQUIRE(history.initialized);
    REQUIRE(history.entries.size() == 1U);
    const auto completed_hash = history.entries[0].completed_file_sha256;
    REQUIRE(completed_hash.has_value());
    CHECK(completed_hash.value_or("") == migration.exact_sha256);
    target.execute_source("CREATE TABLE public.manual_drift(id integer);");
  }

  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::drift);
  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, false}, runtime),
      dbdiff::Error);
  {
    const auto target = postgresql::Database::open(target_locator);
    const auto history = target.read_history();
    REQUIRE(history.entries.size() == 1U);
    CHECK(history.entries[0].completed_file_sha256 == migration.exact_sha256);
  }
  CHECK(scratch_database_count(container, runner) == "0");
}
