#include "dbdiff/docker.hpp"
#include "dbdiff/error.hpp"
#include "dbdiff/hazard.hpp"
#include "dbdiff/lifecycle.hpp"
#include "dbdiff/migration.hpp"
#include "dbdiff/postgresql.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
         "lock_timeout: 250ms\n"
         "statement_timeout: 1m\n";
}

const postgresql::Table* find_table(const postgresql::SchemaSnapshot& snapshot,
                                    const std::string_view schema, const std::string_view name) {
  const auto found = std::ranges::find_if(snapshot.tables, [&](const auto& table) {
    return table.name.schema == schema && table.name.name == name;
  });
  return found == snapshot.tables.end() ? nullptr : &*found;
}

const postgresql::TableConstraint* find_constraint(const postgresql::SchemaSnapshot& snapshot,
                                                   const std::string_view schema,
                                                   const std::string_view table,
                                                   const std::string_view name) {
  const auto found = std::ranges::find_if(snapshot.constraints, [&](const auto& constraint) {
    return constraint.table.schema == schema && constraint.table.name == table &&
           constraint.name == name;
  });
  return found == snapshot.constraints.end() ? nullptr : &*found;
}

const postgresql::Index* find_index(const postgresql::SchemaSnapshot& snapshot,
                                    const std::string_view schema, const std::string_view name) {
  const auto found = std::ranges::find_if(snapshot.indexes, [&](const auto& index) {
    return index.name.schema == schema && index.name.name == name;
  });
  return found == snapshot.indexes.end() ? nullptr : &*found;
}

const postgresql::RowSecurityPolicy* find_policy(const postgresql::SchemaSnapshot& snapshot,
                                                 const std::string_view schema,
                                                 const std::string_view table,
                                                 const std::string_view name) {
  const auto found = std::ranges::find_if(snapshot.policies, [&](const auto& policy) {
    return policy.table.schema == schema && policy.table.name == table && policy.name == name;
  });
  return found == snapshot.policies.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("PostgreSQL lifecycle is isolated, dry-run safe, recoverable, and drift gated",
          "[integration][docker][postgresql][lifecycle][CRE-001][APP-001][APP-002][APP-003]"
          "[APP-004][APP-005][MIG-004][OPS-002][PG-001][PG-002][PG-013][PG-014][PG-015]"
          "[PG-020][SCR-002][SCR-004]") {
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

  {
    auto timed = postgresql::Database::open(
        container.connection_dsn(),
        postgresql::ConnectionSettings{.lock_timeout = 1s, .statement_timeout = 25ms});
    CHECK_THROWS_AS(timed.execute_migration(R"sql(
BEGIN;
DO $body$
BEGIN
  PERFORM pg_catalog.pg_sleep(1);
END;
$body$;
COMMIT;
)sql"),
                    dbdiff::Error);
    CHECK_NOTHROW(timed.introspect({"public"}));
  }

  auto contender = postgresql::Database::open(
      container.connection_dsn(),
      postgresql::ConnectionSettings{.lock_timeout = 25ms, .statement_timeout = 1s});
  const auto acquire_contender_lock = [&contender] {
    const auto lock = contender.acquire_lifecycle_lock();
    static_cast<void>(lock);
  };
  std::optional<postgresql::LifecycleLock> held;
  {
    auto owner = postgresql::Database::open(container.connection_dsn());
    held.emplace(owner.acquire_lifecycle_lock());
    CHECK_NOTHROW(owner.read_history());
    CHECK_THROWS_AS(acquire_contender_lock(), dbdiff::Error);
  }
  CHECK_THROWS_AS(acquire_contender_lock(), dbdiff::Error);
  held.reset();
  CHECK_NOTHROW(acquire_contender_lock());

  {
    auto scratch = postgresql::ScratchDatabase::create(container.connection_dsn());
    scratch.execute_source(R"sql(
CREATE TABLE public.identity_default(
  id bigint GENERATED ALWAYS AS IDENTITY
);
)sql");
    const auto snapshot = scratch.introspect({"public"});
    REQUIRE(snapshot.tables.size() == 1U);
    REQUIRE(snapshot.tables[0].columns.size() == 1U);
    CHECK(snapshot.tables[0].columns[0].identity == postgresql::IdentityGeneration::always);
    CHECK(snapshot.tables[0].columns[0].not_null);
    CHECK_FALSE(snapshot.tables[0].columns[0].default_expression.has_value());
    scratch.close();
  }
  CHECK(scratch_database_count(container, runner) == "0");

  {
    auto scratch = postgresql::ScratchDatabase::create(container.connection_dsn());
    scratch.execute_source(R"sql(
CREATE TABLE public.identity_custom(
  id bigint GENERATED ALWAYS AS IDENTITY (START WITH 42)
);
)sql");
    try {
      static_cast<void>(scratch.introspect({"public"}));
      FAIL("custom identity sequence options were silently accepted");
    } catch (const dbdiff::Error& error) {
      CHECK(error.code() == dbdiff::ErrorCode::unsupported);
      CHECK(std::string_view{error.what()}.find("identity sequence properties") !=
            std::string_view::npos);
    }
    scratch.close();
  }
  CHECK(scratch_database_count(container, runner) == "0");

  dbdiff::test::TempDirectory project;
  project.write("schema/00_core.sql", R"sql(
CREATE TABLE public.accounts(
  id bigint GENERATED ALWAYS AS IDENTITY,
  email text NOT NULL,
  status text NOT NULL DEFAULT 'active',
  CONSTRAINT accounts_pkey PRIMARY KEY(id),
  CONSTRAINT accounts_email_key UNIQUE(email),
  CONSTRAINT accounts_status_check CHECK(status IN ('active', 'disabled'))
);
)sql");
  project.write("schema/10_auth.sql", R"sql(
CREATE SCHEMA app;

CREATE TABLE app.sessions(
  id bigint GENERATED BY DEFAULT AS IDENTITY,
  account_id bigint NOT NULL,
  token text NOT NULL,
  payload text,
  deleted_at timestamp with time zone,
  CONSTRAINT sessions_pkey PRIMARY KEY(id),
  CONSTRAINT sessions_account_fkey
    FOREIGN KEY(account_id) REFERENCES public.accounts(id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX sessions_lookup_idx
  ON app.sessions USING btree(lower(token), account_id DESC)
  INCLUDE(payload)
  NULLS NOT DISTINCT
  WHERE deleted_at IS NULL;

ALTER TABLE app.sessions ENABLE ROW LEVEL SECURITY;
ALTER TABLE app.sessions FORCE ROW LEVEL SECURITY;

CREATE POLICY sessions_visible
  ON app.sessions
  AS PERMISSIVE
  FOR SELECT
  TO PUBLIC
  USING (deleted_at IS NULL);
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
      dbdiff::CreateOptions{
          config, "initial auth", {dbdiff::Hazard::write_lock, dbdiff::Hazard::constraint_scan}},
      runtime);
  REQUIRE(created.created);
  CHECK_FALSE(created.draft);
  CHECK(created.hazards.contains(dbdiff::Hazard::write_lock));
  CHECK(created.hazards.contains(dbdiff::Hazard::constraint_scan));
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

    {
      const auto target_lock = target.acquire_lifecycle_lock();
      CHECK_THROWS_AS(dbdiff::apply_migrations(
                          dbdiff::ApplyOptions{config, true, false, false, false}, runtime),
                      dbdiff::Error);
      static_cast<void>(target_lock);
    }

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

  std::string first_schema_hash;
  std::string first_index_predicate;
  std::string first_policy_expression;
  {
    auto target = postgresql::Database::open(target_locator);
    const auto snapshot = target.introspect({"public", "app"});
    first_schema_hash = snapshot.semantic_hash;

    const auto* accounts = find_table(snapshot, "public", "accounts");
    REQUIRE(accounts != nullptr);
    REQUIRE(accounts->columns.size() == 3U);
    CHECK(accounts->columns[0].name == "id");
    CHECK(accounts->columns[0].not_null);
    CHECK(accounts->columns[0].identity == postgresql::IdentityGeneration::always);
    CHECK_FALSE(accounts->columns[0].default_expression.has_value());

    const auto* sessions = find_table(snapshot, "app", "sessions");
    REQUIRE(sessions != nullptr);
    CHECK(sessions->row_security);
    CHECK(sessions->force_row_security);
    CHECK(sessions->columns[0].identity == postgresql::IdentityGeneration::by_default);

    const auto* primary = find_constraint(snapshot, "public", "accounts", "accounts_pkey");
    REQUIRE(primary != nullptr);
    CHECK(primary->kind == postgresql::ConstraintKind::primary_key);
    REQUIRE(primary->columns.size() == 1U);
    CHECK(primary->columns[0] == "id");

    const auto* unique = find_constraint(snapshot, "public", "accounts", "accounts_email_key");
    REQUIRE(unique != nullptr);
    CHECK(unique->kind == postgresql::ConstraintKind::unique);
    REQUIRE(unique->columns.size() == 1U);
    CHECK(unique->columns[0] == "email");

    const auto* check = find_constraint(snapshot, "public", "accounts", "accounts_status_check");
    REQUIRE(check != nullptr);
    CHECK(check->kind == postgresql::ConstraintKind::check);
    CHECK(check->validated);
    CHECK(check->enforced);

    const auto* foreign = find_constraint(snapshot, "app", "sessions", "sessions_account_fkey");
    REQUIRE(foreign != nullptr);
    CHECK(foreign->kind == postgresql::ConstraintKind::foreign_key);
    REQUIRE(foreign->columns.size() == 1U);
    CHECK(foreign->columns[0] == "account_id");
    REQUIRE(foreign->referenced_table.has_value());
    CHECK(foreign->referenced_table->schema == "public");
    CHECK(foreign->referenced_table->name == "accounts");
    REQUIRE(foreign->referenced_columns.size() == 1U);
    CHECK(foreign->referenced_columns[0] == "id");
    CHECK(foreign->definition.find("ON DELETE CASCADE") != std::string::npos);

    const auto* index = find_index(snapshot, "app", "sessions_lookup_idx");
    REQUIRE(index != nullptr);
    CHECK(index->table.schema == "app");
    CHECK(index->table.name == "sessions");
    CHECK(index->method == "btree");
    CHECK(index->unique);
    CHECK(index->nulls_not_distinct);
    REQUIRE(index->key_expressions.size() == 2U);
    CHECK(index->key_expressions[0] == "lower(token)");
    CHECK(index->key_expressions[1] == "account_id DESC");
    REQUIRE(index->included_columns.size() == 1U);
    CHECK(index->included_columns[0] == "payload");
    REQUIRE(index->predicate.has_value());
    first_index_predicate = *index->predicate;
    CHECK(first_index_predicate.find("deleted_at IS NULL") != std::string::npos);

    const auto* policy = find_policy(snapshot, "app", "sessions", "sessions_visible");
    REQUIRE(policy != nullptr);
    CHECK(policy->command == postgresql::PolicyCommand::select);
    CHECK(policy->permissive);
    REQUIRE(policy->roles.size() == 1U);
    CHECK(policy->roles[0].public_role);
    REQUIRE(policy->using_expression.has_value());
    first_policy_expression = *policy->using_expression;
    CHECK(first_policy_expression.find("deleted_at IS NULL") != std::string::npos);
    CHECK_FALSE(policy->check_expression.has_value());
  }

  project.write("schema/00_core.sql", R"sql(
CREATE TABLE public.accounts(
  id bigint GENERATED ALWAYS AS IDENTITY,
  email text NOT NULL,
  status text NOT NULL DEFAULT 'active',
  CONSTRAINT accounts_pkey PRIMARY KEY(id),
  CONSTRAINT accounts_email_key UNIQUE(email),
  CONSTRAINT accounts_status_check CHECK(status IN ('active', 'disabled', 'locked'))
);
)sql");
  project.write("schema/10_auth.sql", R"sql(
CREATE SCHEMA app;

CREATE TABLE app.sessions(
  id bigint GENERATED BY DEFAULT AS IDENTITY,
  account_id bigint NOT NULL,
  token text NOT NULL,
  payload text,
  deleted_at timestamp with time zone,
  device_label text,
  CONSTRAINT sessions_pkey PRIMARY KEY(id),
  CONSTRAINT sessions_account_fkey
    FOREIGN KEY(account_id) REFERENCES public.accounts(id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX sessions_lookup_idx
  ON app.sessions USING btree(lower(token), account_id DESC)
  INCLUDE(payload)
  NULLS NOT DISTINCT
  WHERE deleted_at IS NULL AND device_label IS NOT NULL;

ALTER TABLE app.sessions ENABLE ROW LEVEL SECURITY;
ALTER TABLE app.sessions FORCE ROW LEVEL SECURITY;

CREATE POLICY sessions_visible
  ON app.sessions
  AS RESTRICTIVE
  FOR SELECT
  TO PUBLIC
  USING (deleted_at IS NULL AND device_label IS NOT NULL);
)sql");

  expose_target = false;
  const auto target_lookups_before_second_create = target_locator_lookups;
  const auto second = dbdiff::create_migration(
      dbdiff::CreateOptions{
          config, "session devices", {dbdiff::Hazard::write_lock, dbdiff::Hazard::constraint_scan}},
      runtime);
  REQUIRE(second.created);
  CHECK_FALSE(second.draft);
  CHECK(target_locator_lookups == target_lookups_before_second_create);
  CHECK(second.hazards.contains(dbdiff::Hazard::write_lock));
  CHECK(second.hazards.contains(dbdiff::Hazard::constraint_scan));
  CHECK(scratch_database_count(container, runner) == "0");

  const auto second_migration =
      dbdiff::load_migration(second.file, dbdiff::BackendKind::postgresql);
  const auto& second_sql = second_migration.sql;
  const auto drop_policy =
      second_sql.find("DROP POLICY \"sessions_visible\" ON \"app\".\"sessions\"");
  const auto drop_index = second_sql.find("DROP INDEX \"app\".\"sessions_lookup_idx\" RESTRICT");
  const auto add_column = second_sql.find("ADD COLUMN \"device_label\" text");
  const auto create_index = second_sql.find("CREATE UNIQUE INDEX \"sessions_lookup_idx\"");
  const auto create_policy = second_sql.find("CREATE POLICY \"sessions_visible\"");
  const auto drop_check = second_sql.find("DROP CONSTRAINT \"accounts_status_check\" RESTRICT");
  const auto create_check = second_sql.find("ADD CONSTRAINT \"accounts_status_check\"");
  REQUIRE(drop_policy != std::string::npos);
  REQUIRE(drop_index != std::string::npos);
  REQUIRE(add_column != std::string::npos);
  REQUIRE(create_index != std::string::npos);
  REQUIRE(create_policy != std::string::npos);
  REQUIRE(drop_check != std::string::npos);
  REQUIRE(create_check != std::string::npos);
  CHECK(drop_policy < add_column);
  CHECK(drop_index < add_column);
  CHECK(add_column < create_index);
  CHECK(add_column < create_policy);
  CHECK(drop_check < create_check);

  expose_target = true;
  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::pending);
  const auto second_applied =
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, false}, runtime);
  CHECK(second_applied.pending == 1U);
  CHECK(second_applied.applied == 1U);
  const auto status_after_second = dbdiff::project_status(config, runtime);
  CHECK(status_after_second.status == dbdiff::ProjectStatus::converged);
  CHECK(status_after_second.applied == 2U);
  CHECK(status_after_second.total == 2U);

  {
    auto target = postgresql::Database::open(target_locator);
    const auto snapshot = target.introspect({"public", "app"});
    CHECK(snapshot.semantic_hash != first_schema_hash);

    const auto* sessions = find_table(snapshot, "app", "sessions");
    REQUIRE(sessions != nullptr);
    REQUIRE(sessions->columns.size() == 6U);
    CHECK(sessions->columns.back().name == "device_label");
    CHECK_FALSE(sessions->columns.back().not_null);

    const auto* index = find_index(snapshot, "app", "sessions_lookup_idx");
    REQUIRE(index != nullptr);
    REQUIRE(index->predicate.has_value());
    CHECK(*index->predicate != first_index_predicate);
    CHECK(index->predicate->find("device_label IS NOT NULL") != std::string::npos);

    const auto* policy = find_policy(snapshot, "app", "sessions", "sessions_visible");
    REQUIRE(policy != nullptr);
    CHECK_FALSE(policy->permissive);
    REQUIRE(policy->using_expression.has_value());
    CHECK(*policy->using_expression != first_policy_expression);
    CHECK(policy->using_expression->find("device_label IS NOT NULL") != std::string::npos);
  }

  expose_target = false;
  const auto lookups_before_noop = target_locator_lookups;
  const auto no_change = dbdiff::create_migration(
      dbdiff::CreateOptions{
          config, "no changes", {dbdiff::Hazard::write_lock, dbdiff::Hazard::constraint_scan}},
      runtime);
  CHECK_FALSE(no_change.created);
  CHECK(target_locator_lookups == lookups_before_noop);
  CHECK(scratch_database_count(container, runner) == "0");
  expose_target = true;

  {
    auto target = postgresql::Database::open(target_locator);
    const auto history = target.read_history();
    REQUIRE(history.initialized);
    REQUIRE(history.entries.size() == 2U);
    const auto completed_hash = history.entries[0].completed_file_sha256;
    REQUIRE(completed_hash.has_value());
    CHECK(completed_hash.value_or("") == migration.exact_sha256);
    CHECK(history.entries[1].completed_file_sha256 == second_migration.exact_sha256);
    target.execute_source("CREATE TABLE public.manual_drift(id integer);");
  }

  CHECK(dbdiff::project_status(config, runtime).status == dbdiff::ProjectStatus::drift);
  CHECK_THROWS_AS(
      dbdiff::apply_migrations(dbdiff::ApplyOptions{config, false, false, false, false}, runtime),
      dbdiff::Error);
  {
    const auto target = postgresql::Database::open(target_locator);
    const auto history = target.read_history();
    REQUIRE(history.entries.size() == 2U);
    CHECK(history.entries[0].completed_file_sha256 == migration.exact_sha256);
    CHECK(history.entries[1].completed_file_sha256 == second_migration.exact_sha256);
  }
  CHECK(scratch_database_count(container, runner) == "0");
}
