#include "dbdiff/error.hpp"
#include "dbdiff/postgresql.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace pg = dbdiff::postgresql;

namespace {

constexpr pg::ServerVersion pg15{150017, 15, 17};

[[nodiscard]] pg::SchemaSnapshot one_table(std::vector<pg::Column> columns) {
  return pg::SchemaSnapshot{
      .server_version = pg15,
      .schemas = {"public"},
      .tables = {pg::Table{.name = {"public", "items"}, .columns = std::move(columns)}},
  };
}

[[nodiscard]] bool has_hazard(const pg::MigrationPlan& migration_plan,
                              const dbdiff::Hazard hazard) {
  return std::ranges::any_of(
      migration_plan.operations,
      [hazard](const dbdiff::Operation& operation) { return operation.hazards.contains(hazard); });
}

} // namespace

static_assert(std::is_move_constructible_v<pg::Database>);
static_assert(!std::is_copy_constructible_v<pg::Database>);

TEST_CASE("PostgreSQL scanner preserves dollar quoted bodies and exact boundaries",
          "[unit][postgresql][scanner][SQL-001]") {
  const std::string sql = R"sql(-- leading comment with ;
CREATE FUNCTION public.audit(value text) RETURNS text
LANGUAGE plpgsql AS $body$
BEGIN
  /* nested ; /* comment ; */ still a comment */
  RETURN value || ';';
END;
$body$;
BEGIN;
INSERT INTO public.events(message) VALUES (E'quote\'; and semicolon;');
SAVEPOINT before_more;
ROLLBACK TO SAVEPOINT before_more;
COMMIT
)sql";

  const auto statements = pg::scan_statements(sql);
  REQUIRE(statements.size() == 6U);
  CHECK(statements[0].kind == dbdiff::StatementKind::ddl);
  CHECK(statements[1].kind == dbdiff::StatementKind::begin);
  CHECK(statements[2].kind == dbdiff::StatementKind::dml);
  CHECK(statements[3].kind == dbdiff::StatementKind::savepoint);
  CHECK(statements[4].kind == dbdiff::StatementKind::rollback_to_savepoint);
  CHECK(statements[5].kind == dbdiff::StatementKind::commit);

  const auto function =
      std::string_view{sql}.substr(statements[0].begin, statements[0].end - statements[0].begin);
  CHECK(function.starts_with("-- leading comment"));
  CHECK(function.ends_with("$body$;"));

  const auto parsed = pg::parse_migration(sql);
  REQUIRE(parsed.units.size() == 2U);
  CHECK_FALSE(parsed.units[0].explicit_transaction);
  CHECK(parsed.units[1].explicit_transaction);
  CHECK(parsed.units[1].statements.size() == 5U);
}

TEST_CASE("PostgreSQL scanner rejects malformed and non-resumable scripts",
          "[unit][postgresql][scanner][SQL-001][SQL-003]") {
  const auto ordinary_backslash = pg::scan_statements(R"sql(SELECT '\'; COMMIT;)sql");
  REQUIRE(ordinary_backslash.size() == 2U);
  CHECK(ordinary_backslash[1].kind == dbdiff::StatementKind::commit);

  CHECK_THROWS_AS(pg::scan_statements("SELECT $tag$unfinished"), dbdiff::Error);
  CHECK_THROWS_AS(pg::scan_statements("/* outer /* inner */"), dbdiff::Error);
  CHECK_THROWS_AS(pg::scan_statements("SELECT 'unfinished"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("INSERT INTO things VALUES (1);"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("BEGIN; CREATE TABLE things(id integer);"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("ROLLBACK;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("SELECT 1;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("SET standard_conforming_strings = off;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("BEGIN; COMMIT AND CHAIN;"), dbdiff::Error);
}

TEST_CASE("PostgreSQL semantic hashes are normalized and version-patch independent",
          "[unit][postgresql][hash]") {
  auto first = pg::SchemaSnapshot{
      .server_version = pg15,
      .schemas = {"utils", "public"},
      .tables =
          {
              pg::Table{
                  .name = {"public", "items"},
                  .columns =
                      {
                          pg::Column{.position = 9, .name = "name", .type = "text"},
                          pg::Column{
                              .position = 3, .name = "id", .type = "bigint", .not_null = true},
                      },
              },
          },
  };
  auto second = pg::SchemaSnapshot{
      .server_version = {150099, 15, 99},
      .schemas = {"public", "utils"},
      .tables =
          {
              pg::Table{
                  .name = {"public", "items"},
                  .columns =
                      {
                          pg::Column{
                              .position = 1, .name = "id", .type = "bigint", .not_null = true},
                          pg::Column{.position = 2, .name = "name", .type = "text"},
                      },
              },
          },
  };

  CHECK(pg::semantic_hash(first) == pg::semantic_hash(second));
  const auto normalized = pg::normalize_snapshot(std::move(first));
  CHECK(normalized.semantic_hash.size() == 64U);
  REQUIRE(normalized.tables.size() == 1U);
  REQUIRE(normalized.tables[0].columns.size() == 2U);
  CHECK(normalized.tables[0].columns[0].position == 1);
  CHECK(normalized.tables[0].columns[1].position == 2);

  second.tables[0].columns[1].not_null = true;
  CHECK(normalized.semantic_hash != pg::semantic_hash(second));
}

TEST_CASE("PostgreSQL planner creates schemas and tables in dependency order",
          "[unit][postgresql][planner][MIG-005][PG-010][PG-013]") {
  const pg::SchemaSnapshot from{.server_version = pg15, .schemas = {"public"}};
  const pg::SchemaSnapshot to{
      .server_version = pg15,
      .schemas = {"auth", "public"},
      .tables =
          {
              pg::Table{
                  .name = {"auth", "users"},
                  .columns =
                      {
                          pg::Column{.position = 1,
                                     .name = "id",
                                     .type = "bigint",
                                     .not_null = true,
                                     .identity = pg::IdentityGeneration::always},
                          pg::Column{
                              .position = 2, .name = "email", .type = "text", .not_null = true},
                      },
              },
          },
  };

  const auto migration_plan = pg::plan(from, to);
  CHECK_FALSE(migration_plan.draft);
  REQUIRE(migration_plan.operations.size() == 2U);
  const auto sql = pg::render_plan(migration_plan);
  CHECK(sql.starts_with("BEGIN;\n"));
  CHECK(sql.ends_with("COMMIT;\n"));
  const auto schema_position = sql.find("CREATE SCHEMA \"auth\"");
  const auto table_position = sql.find("CREATE TABLE \"auth\".\"users\"");
  REQUIRE(schema_position != std::string::npos);
  REQUIRE(table_position != std::string::npos);
  CHECK(schema_position < table_position);
  CHECK(sql.find("GENERATED ALWAYS AS IDENTITY") != std::string::npos);
}

TEST_CASE("PostgreSQL planner handles supported column changes with hazards",
          "[unit][postgresql][planner][PG-013]") {
  const auto from = one_table({
      pg::Column{.position = 1, .name = "id", .type = "bigint", .not_null = true},
      pg::Column{.position = 2, .name = "label", .type = "text"},
      pg::Column{.position = 3, .name = "legacy", .type = "text"},
  });
  const auto to = one_table({
      pg::Column{.position = 1, .name = "id", .type = "bigint", .not_null = true},
      pg::Column{.position = 2,
                 .name = "label",
                 .type = "text",
                 .not_null = true,
                 .default_expression = "'new'::text"},
      pg::Column{.position = 3,
                 .name = "created_at",
                 .type = "timestamp with time zone",
                 .not_null = true,
                 .default_expression = "CURRENT_TIMESTAMP"},
  });

  const auto migration_plan = pg::plan(from, to);
  CHECK_FALSE(migration_plan.draft);
  const auto sql = pg::render_plan(migration_plan);
  CHECK(sql.find("DROP COLUMN \"legacy\" RESTRICT") != std::string::npos);
  CHECK(sql.find("ADD COLUMN \"created_at\" timestamp with time zone") != std::string::npos);
  CHECK(sql.find("ALTER COLUMN \"label\" SET DEFAULT 'new'::text") != std::string::npos);
  CHECK(sql.find("ALTER COLUMN \"label\" SET NOT NULL") != std::string::npos);
  CHECK(has_hazard(migration_plan, dbdiff::Hazard::data_loss));
  CHECK(has_hazard(migration_plan, dbdiff::Hazard::constraint_scan));
  CHECK(has_hazard(migration_plan, dbdiff::Hazard::table_rewrite));

  const auto reverse_plan = pg::plan(to, from);
  const auto reverse_sql = pg::render_plan(reverse_plan);
  CHECK(reverse_sql.find("ALTER COLUMN \"label\" DROP DEFAULT") != std::string::npos);
  CHECK(reverse_sql.find("ALTER COLUMN \"label\" DROP NOT NULL") != std::string::npos);

  CHECK(pg::render_plan(pg::plan(from, to)) == sql);
}

TEST_CASE("PostgreSQL planner drops tables before their schemas",
          "[unit][postgresql][planner][PG-010][PG-013]") {
  const pg::SchemaSnapshot from{
      .server_version = pg15,
      .schemas = {"legacy", "public"},
      .tables =
          {
              pg::Table{
                  .name = {"legacy", "events"},
                  .columns = {pg::Column{.position = 1, .name = "id", .type = "bigint"}},
              },
          },
  };
  const pg::SchemaSnapshot to{.server_version = pg15, .schemas = {"public"}};

  const auto migration_plan = pg::plan(from, to);
  CHECK_FALSE(migration_plan.draft);
  CHECK(has_hazard(migration_plan, dbdiff::Hazard::data_loss));
  const auto sql = pg::render_plan(migration_plan);
  const auto table_position = sql.find("DROP TABLE \"legacy\".\"events\" RESTRICT");
  const auto schema_position = sql.find("DROP SCHEMA \"legacy\" RESTRICT");
  REQUIRE(table_position != std::string::npos);
  REQUIRE(schema_position != std::string::npos);
  CHECK(table_position < schema_position);
}

TEST_CASE("PostgreSQL planner drafts ambiguous and data-dependent changes",
          "[unit][postgresql][planner][PLN-002][PLN-004]") {
  SECTION("column rename") {
    const auto from = one_table({pg::Column{.position = 1, .name = "old_name", .type = "text"}});
    const auto to = one_table({pg::Column{.position = 1, .name = "new_name", .type = "text"}});
    const auto migration_plan = pg::plan(from, to);
    CHECK(migration_plan.draft);
    CHECK(migration_plan.operations.empty());
    CHECK(pg::render_plan(migration_plan).find("RENAME COLUMN") != std::string::npos);
  }

  SECTION("table rename") {
    const auto from =
        one_table({pg::Column{.position = 1, .name = "id", .type = "bigint", .not_null = true}});
    auto to = from;
    to.tables[0].name.name = "renamed_items";
    const auto migration_plan = pg::plan(from, to);
    CHECK(migration_plan.draft);
    CHECK(migration_plan.operations.empty());
    CHECK(pg::render_plan(migration_plan).find("ALTER TABLE ... RENAME") != std::string::npos);
  }

  SECTION("type conversion") {
    const auto from = one_table({pg::Column{.position = 1, .name = "value", .type = "text"}});
    const auto to = one_table({pg::Column{.position = 1, .name = "value", .type = "integer"}});
    const auto migration_plan = pg::plan(from, to);
    CHECK(migration_plan.draft);
    CHECK(migration_plan.operations.empty());
    CHECK(pg::render_plan(migration_plan).find("USING expression") != std::string::npos);
  }

  SECTION("not-null append without a backfill") {
    const auto from = one_table({pg::Column{.position = 1, .name = "id", .type = "bigint"}});
    const auto to = one_table({
        pg::Column{.position = 1, .name = "id", .type = "bigint"},
        pg::Column{.position = 2, .name = "required", .type = "text", .not_null = true},
    });
    const auto migration_plan = pg::plan(from, to);
    CHECK(migration_plan.draft);
    CHECK(migration_plan.operations.empty());
    CHECK(pg::render_plan(migration_plan).find("backfill/default") != std::string::npos);
  }

  SECTION("server major mismatch") {
    const pg::SchemaSnapshot from{.server_version = pg15, .schemas = {"public"}};
    const pg::SchemaSnapshot to{.server_version = {160000, 16, 0}, .schemas = {"public"}};
    CHECK_THROWS_AS(pg::plan(from, to), dbdiff::Error);
  }
}
