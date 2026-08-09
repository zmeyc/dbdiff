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

[[nodiscard]] std::size_t occurrences(const std::string_view text, const std::string_view needle) {
  std::size_t count = 0U;
  std::size_t position = 0U;
  while ((position = text.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
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

TEST_CASE("PostgreSQL scanner rejects data-bearing sources and server-wide migrations",
          "[unit][postgresql][scanner][SRC-005][SQL-001][PG-004]") {
  CHECK_NOTHROW(
      pg::validate_source("CREATE TABLE public.items ("
                          "id bigint, doubled bigint GENERATED ALWAYS AS (id * 2) STORED);"));
  CHECK_THROWS_AS(pg::validate_source("CREATE TABLE public.items AS SELECT 1 AS id;"),
                  dbdiff::Error);
  CHECK_THROWS_AS(
      pg::validate_source("CREATE UNLOGGED TABLE public.items WITH (fillfactor = 80) AS "
                          "SELECT 1 AS id;"),
      dbdiff::Error);

  CHECK_THROWS_AS(pg::parse_migration("CREATE DATABASE escaped;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("ALTER ROLE app LOGIN;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("DROP TABLESPACE shared_space;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("ALTER SYSTEM SET work_mem = '1GB';"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("CREATE SUBSCRIPTION outside CONNECTION 'host=remote' "
                                      "PUBLICATION changes;"),
                  dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("CHECKPOINT;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("GRANT cluster_admin TO app_user;"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("COMMENT ON DATABASE postgres IS 'changed';"), dbdiff::Error);
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

TEST_CASE("PostgreSQL relational objects have deterministic semantic identities",
          "[unit][postgresql][planner][hash][PG-001][PG-004][PG-014][PG-015][PG-020]") {
  auto snapshot = pg::SchemaSnapshot{
      .server_version = pg15,
      .schemas = {"public"},
      .tables =
          {
              pg::Table{
                  .name = {"public", "items"},
                  .row_security = true,
                  .columns =
                      {
                          pg::Column{
                              .position = 1, .name = "id", .type = "bigint", .not_null = true},
                          pg::Column{.position = 2, .name = "title", .type = "text"},
                          pg::Column{.position = 3, .name = "payload", .type = "text"},
                      },
              },
          },
      .constraints =
          {
              pg::TableConstraint{
                  .table = {"public", "items"},
                  .name = "items_title_check",
                  .kind = pg::ConstraintKind::check,
                  .columns = {"title"},
                  .definition = "CHECK ((length(title) > 0))",
              },
              pg::TableConstraint{
                  .table = {"public", "items"},
                  .name = "items_pkey",
                  .kind = pg::ConstraintKind::primary_key,
                  .columns = {"id"},
                  .definition = "PRIMARY KEY (id)",
              },
          },
      .indexes =
          {
              pg::Index{
                  .name = {"public", "items_search_idx"},
                  .table = {"public", "items"},
                  .method = "btree",
                  .unique = true,
                  .nulls_not_distinct = true,
                  .key_expressions = {"lower(title)"},
                  .included_columns = {"payload"},
                  .predicate = "(title IS NOT NULL)",
              },
          },
      .policies =
          {
              pg::RowSecurityPolicy{
                  .table = {"public", "items"},
                  .name = "items_read",
                  .command = pg::PolicyCommand::select,
                  .roles =
                      {
                          pg::PolicyRole{.public_role = true},
                      },
                  .using_expression = "(id > 0)",
              },
          },
  };

  const auto normalized = pg::normalize_snapshot(snapshot);
  REQUIRE(normalized.constraints.size() == 2U);
  CHECK(normalized.constraints[0].name == "items_pkey");
  REQUIRE(normalized.policies[0].roles.size() == 1U);
  CHECK(normalized.policies[0].roles[0].public_role);
  CHECK(normalized.semantic_hash == pg::semantic_hash(normalized));

  snapshot.indexes[0].predicate = "(title <> '')";
  CHECK(pg::semantic_hash(snapshot) != normalized.semantic_hash);
  snapshot.indexes[0].predicate = "(title IS NOT NULL)";
  snapshot.constraints[0].validated = false;
  CHECK(pg::semantic_hash(snapshot) != normalized.semantic_hash);
  snapshot.constraints[0].validated = true;
  snapshot.policies[0].permissive = false;
  CHECK(pg::semantic_hash(snapshot) != normalized.semantic_hash);

  snapshot.unsupported_objects = {
      pg::UnsupportedCatalogObject{.kind = "view", .identity = "public.report"}};
  CHECK_THROWS_AS(pg::normalize_snapshot(std::move(snapshot)), dbdiff::Error);

  auto external_role = normalized;
  external_role.policies[0].roles = {pg::PolicyRole{.name = "app_reader"}};
  CHECK_THROWS_AS(pg::normalize_snapshot(std::move(external_role)), dbdiff::Error);
}

TEST_CASE("PostgreSQL planner creates keys advanced indexes cyclic foreign keys and policies",
          "[unit][postgresql][planner][PG-001][PG-002][PG-014][PG-015][PG-020]") {
  const pg::SchemaSnapshot from{.server_version = pg15, .schemas = {"public"}};
  const pg::SchemaSnapshot to{
      .server_version = pg15,
      .schemas = {"public"},
      .tables =
          {
              pg::Table{
                  .name = {"public", "accounts"},
                  .columns =
                      {
                          pg::Column{
                              .position = 1, .name = "id", .type = "bigint", .not_null = true},
                          pg::Column{.position = 2, .name = "featured_entry_id", .type = "bigint"},
                      },
              },
              pg::Table{
                  .name = {"public", "entries"},
                  .row_security = true,
                  .columns =
                      {
                          pg::Column{
                              .position = 1, .name = "id", .type = "bigint", .not_null = true},
                          pg::Column{.position = 2, .name = "account_id", .type = "bigint"},
                          pg::Column{.position = 3, .name = "title", .type = "text"},
                          pg::Column{.position = 4, .name = "payload", .type = "text"},
                          pg::Column{.position = 5, .name = "deleted_at", .type = "timestamp"},
                      },
              },
          },
      .constraints =
          {
              pg::TableConstraint{
                  .table = {"public", "accounts"},
                  .name = "accounts_pkey",
                  .kind = pg::ConstraintKind::primary_key,
                  .columns = {"id"},
                  .definition = "PRIMARY KEY (id)",
              },
              pg::TableConstraint{
                  .table = {"public", "accounts"},
                  .name = "accounts_featured_entry_fkey",
                  .kind = pg::ConstraintKind::foreign_key,
                  .columns = {"featured_entry_id"},
                  .definition = "FOREIGN KEY (featured_entry_id) REFERENCES public.entries(id)",
                  .referenced_table = pg::QualifiedName{"public", "entries"},
                  .referenced_columns = {"id"},
              },
              pg::TableConstraint{
                  .table = {"public", "entries"},
                  .name = "entries_pkey",
                  .kind = pg::ConstraintKind::primary_key,
                  .columns = {"id"},
                  .definition = "PRIMARY KEY (id)",
              },
              pg::TableConstraint{
                  .table = {"public", "entries"},
                  .name = "entries_account_fkey",
                  .kind = pg::ConstraintKind::foreign_key,
                  .columns = {"account_id"},
                  .definition = "FOREIGN KEY (account_id) REFERENCES public.accounts(id)",
                  .referenced_table = pg::QualifiedName{"public", "accounts"},
                  .referenced_columns = {"id"},
              },
          },
      .indexes =
          {
              pg::Index{
                  .name = {"public", "entries_search_idx"},
                  .table = {"public", "entries"},
                  .method = "btree",
                  .unique = true,
                  .nulls_not_distinct = true,
                  .key_expressions = {"lower(title)", "account_id DESC"},
                  .included_columns = {"payload"},
                  .predicate = "(deleted_at IS NULL)",
              },
          },
      .policies =
          {
              pg::RowSecurityPolicy{
                  .table = {"public", "entries"},
                  .name = "entries_read",
                  .command = pg::PolicyCommand::select,
                  .roles = {pg::PolicyRole{.public_role = true}},
                  .using_expression = "((account_id)::text = CURRENT_USER)",
              },
          },
  };

  const auto migration_plan = pg::plan(from, to);
  CHECK_FALSE(migration_plan.draft);
  CHECK(has_hazard(migration_plan, dbdiff::Hazard::constraint_scan));
  CHECK(has_hazard(migration_plan, dbdiff::Hazard::write_lock));
  const auto sql = pg::render_plan(migration_plan);
  const auto accounts_table = sql.find("CREATE TABLE \"public\".\"accounts\"");
  const auto entries_table = sql.find("CREATE TABLE \"public\".\"entries\"");
  const auto accounts_key = sql.find("ADD CONSTRAINT \"accounts_pkey\"");
  const auto entries_key = sql.find("ADD CONSTRAINT \"entries_pkey\"");
  const auto accounts_fk = sql.find("ADD CONSTRAINT \"accounts_featured_entry_fkey\"");
  const auto entries_fk = sql.find("ADD CONSTRAINT \"entries_account_fkey\"");
  REQUIRE(accounts_table != std::string::npos);
  REQUIRE(entries_table != std::string::npos);
  REQUIRE(accounts_key != std::string::npos);
  REQUIRE(entries_key != std::string::npos);
  REQUIRE(accounts_fk != std::string::npos);
  REQUIRE(entries_fk != std::string::npos);
  CHECK(accounts_table < accounts_fk);
  CHECK(entries_table < accounts_fk);
  CHECK(accounts_key < entries_fk);
  CHECK(entries_key < accounts_fk);
  CHECK(sql.find("CREATE UNIQUE INDEX \"entries_search_idx\" ON \"public\".\"entries\" USING "
                 "\"btree\" (lower(title), account_id DESC) "
                 "INCLUDE (\"payload\") NULLS NOT DISTINCT WHERE (deleted_at IS NULL);") !=
        std::string::npos);
  CHECK(sql.find("CREATE POLICY \"entries_read\" ON \"public\".\"entries\" AS PERMISSIVE "
                 "FOR SELECT TO PUBLIC USING (((account_id)::text = CURRENT_USER));") !=
        std::string::npos);
  CHECK(occurrences(sql, "CREATE INDEX") == 0U);
  CHECK(occurrences(sql, "CREATE UNIQUE INDEX") == 1U);
}

TEST_CASE("PostgreSQL planner replaces referenced keys around foreign keys",
          "[unit][postgresql][planner][PG-002][PG-014]") {
  const auto tables = std::vector<pg::Table>{
      pg::Table{
          .name = {"public", "users"},
          .columns =
              {
                  pg::Column{.position = 1, .name = "email", .type = "text", .not_null = true},
              },
      },
      pg::Table{
          .name = {"public", "sessions"},
          .columns =
              {
                  pg::Column{.position = 1, .name = "user_email", .type = "text"},
              },
      },
  };
  const auto foreign_key = pg::TableConstraint{
      .table = {"public", "sessions"},
      .name = "sessions_user_email_fkey",
      .kind = pg::ConstraintKind::foreign_key,
      .columns = {"user_email"},
      .definition = "FOREIGN KEY (user_email) REFERENCES public.users(email)",
      .referenced_table = pg::QualifiedName{"public", "users"},
      .referenced_columns = {"email"},
  };
  const pg::SchemaSnapshot from{
      .server_version = pg15,
      .schemas = {"public"},
      .tables = tables,
      .constraints =
          {
              pg::TableConstraint{
                  .table = {"public", "users"},
                  .name = "users_email_key",
                  .kind = pg::ConstraintKind::unique,
                  .columns = {"email"},
                  .definition = "UNIQUE (email)",
              },
              foreign_key,
          },
  };
  auto to = from;
  to.constraints[0].definition = "UNIQUE NULLS NOT DISTINCT (email)";

  const auto sql = pg::render_plan(pg::plan(from, to));
  const auto drop_fk = sql.find("DROP CONSTRAINT \"sessions_user_email_fkey\"");
  const auto drop_key = sql.find("DROP CONSTRAINT \"users_email_key\"");
  const auto create_key = sql.find("ADD CONSTRAINT \"users_email_key\"");
  const auto create_fk = sql.find("ADD CONSTRAINT \"sessions_user_email_fkey\"");
  REQUIRE(drop_fk != std::string::npos);
  REQUIRE(drop_key != std::string::npos);
  REQUIRE(create_key != std::string::npos);
  REQUIRE(create_fk != std::string::npos);
  CHECK(drop_fk < drop_key);
  CHECK(drop_key < create_key);
  CHECK(create_key < create_fk);
}

TEST_CASE("PostgreSQL 18 named not-null constraints are ordered around primary keys",
          "[unit][postgresql][planner][PG-002][PG-014]") {
  constexpr pg::ServerVersion pg18{180000, 18, 0};
  const auto table = pg::Table{
      .name = {"public", "items"},
      .columns = {pg::Column{.position = 1, .name = "id", .type = "bigint", .not_null = true}},
  };

  SECTION("create not-null before a lexically earlier primary key") {
    const pg::SchemaSnapshot from{.server_version = pg18, .schemas = {"public"}};
    const pg::SchemaSnapshot to{
        .server_version = pg18,
        .schemas = {"public"},
        .tables = {table},
        .constraints =
            {
                pg::TableConstraint{
                    .table = {"public", "items"},
                    .name = "a_items_pkey",
                    .kind = pg::ConstraintKind::primary_key,
                    .columns = {"id"},
                    .definition = "PRIMARY KEY (id)",
                },
                pg::TableConstraint{
                    .table = {"public", "items"},
                    .name = "z_items_id_not_null",
                    .kind = pg::ConstraintKind::not_null,
                    .columns = {"id"},
                    .definition = "NOT NULL id",
                },
            },
    };

    const auto sql = pg::render_plan(pg::plan(from, to));
    const auto not_null = sql.find("ADD CONSTRAINT \"z_items_id_not_null\"");
    const auto primary_key = sql.find("ADD CONSTRAINT \"a_items_pkey\"");
    REQUIRE(not_null != std::string::npos);
    REQUIRE(primary_key != std::string::npos);
    CHECK(not_null < primary_key);
  }

  SECTION("drop a lexically later primary key before not-null") {
    const pg::SchemaSnapshot from{
        .server_version = pg18,
        .schemas = {"public"},
        .tables = {table},
        .constraints =
            {
                pg::TableConstraint{
                    .table = {"public", "items"},
                    .name = "a_items_id_not_null",
                    .kind = pg::ConstraintKind::not_null,
                    .columns = {"id"},
                    .definition = "NOT NULL id",
                },
                pg::TableConstraint{
                    .table = {"public", "items"},
                    .name = "z_items_pkey",
                    .kind = pg::ConstraintKind::primary_key,
                    .columns = {"id"},
                    .definition = "PRIMARY KEY (id)",
                },
            },
    };
    const pg::SchemaSnapshot to{
        .server_version = pg18,
        .schemas = {"public"},
        .tables = {pg::Table{
            .name = {"public", "items"},
            .columns = {pg::Column{.position = 1, .name = "id", .type = "bigint"}},
        }},
    };

    const auto sql = pg::render_plan(pg::plan(from, to));
    const auto primary_key = sql.find("DROP CONSTRAINT \"z_items_pkey\"");
    const auto not_null = sql.find("DROP CONSTRAINT \"a_items_id_not_null\"");
    REQUIRE(primary_key != std::string::npos);
    REQUIRE(not_null != std::string::npos);
    CHECK(primary_key < not_null);
  }
}

TEST_CASE("PostgreSQL planner drops policies and indexes before dependent columns",
          "[unit][postgresql][planner][PG-002][PG-015][PG-020]") {
  const pg::SchemaSnapshot from{
      .server_version = pg15,
      .schemas = {"public"},
      .tables =
          {
              pg::Table{
                  .name = {"public", "items"},
                  .row_security = true,
                  .columns =
                      {
                          pg::Column{.position = 1, .name = "id", .type = "bigint"},
                          pg::Column{.position = 2, .name = "legacy", .type = "text"},
                      },
              },
          },
      .indexes =
          {
              pg::Index{
                  .name = {"public", "items_legacy_idx"},
                  .table = {"public", "items"},
                  .key_expressions = {"legacy"},
                  .predicate = "(legacy IS NOT NULL)",
              },
          },
      .policies =
          {
              pg::RowSecurityPolicy{
                  .table = {"public", "items"},
                  .name = "items_legacy",
                  .command = pg::PolicyCommand::select,
                  .roles = {pg::PolicyRole{.public_role = true}},
                  .using_expression = "(legacy IS NOT NULL)",
              },
          },
  };
  const pg::SchemaSnapshot to{
      .server_version = pg15,
      .schemas = {"public"},
      .tables =
          {
              pg::Table{
                  .name = {"public", "items"},
                  .row_security = true,
                  .columns = {pg::Column{.position = 1, .name = "id", .type = "bigint"}},
              },
          },
  };

  const auto sql = pg::render_plan(pg::plan(from, to));
  const auto drop_policy = sql.find("DROP POLICY \"items_legacy\"");
  const auto drop_index = sql.find("DROP INDEX \"public\".\"items_legacy_idx\"");
  const auto drop_column = sql.find("DROP COLUMN \"legacy\"");
  REQUIRE(drop_policy != std::string::npos);
  REQUIRE(drop_index != std::string::npos);
  REQUIRE(drop_column != std::string::npos);
  CHECK(drop_policy < drop_column);
  CHECK(drop_index < drop_column);
}
