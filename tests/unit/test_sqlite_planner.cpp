#include "dbdiff/hazard.hpp"
#include "dbdiff/sqlite.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] dbdiff::sqlite::SchemaSnapshot snapshot(const std::string_view sql) {
  auto database = dbdiff::sqlite::Database::temporary();
  if (!sql.empty()) {
    database.execute_source(sql);
  }
  return database.inspect();
}

[[nodiscard]] bool has_hazard(const dbdiff::sqlite::Plan& plan, const dbdiff::Hazard hazard) {
  return plan.hazards.contains(hazard);
}

[[nodiscard]] std::string query_text(const std::filesystem::path& path,
                                     const std::string_view sql) {
  sqlite3* database = nullptr;
  if (sqlite3_open_v2(path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    const std::string message =
        database == nullptr ? "cannot open SQLite test database" : sqlite3_errmsg(database);
    if (database != nullptr) {
      static_cast<void>(sqlite3_close(database));
    }
    throw std::runtime_error{message};
  }

  sqlite3_stmt* statement = nullptr;
  const auto prepare =
      sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
  if (prepare != SQLITE_OK) {
    const std::string message = sqlite3_errmsg(database);
    static_cast<void>(sqlite3_close(database));
    throw std::runtime_error{message};
  }
  if (sqlite3_step(statement) != SQLITE_ROW) {
    static_cast<void>(sqlite3_finalize(statement));
    static_cast<void>(sqlite3_close(database));
    throw std::runtime_error{"SQLite test query returned no row"};
  }
  const auto* value = sqlite3_column_text(statement, 0);
  const auto size = sqlite3_column_bytes(statement, 0);
  const std::string result{reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
  static_cast<void>(sqlite3_finalize(statement));
  static_cast<void>(sqlite3_close(database));
  return result;
}

} // namespace

TEST_CASE("SQLite planner creates and drops complete schema object sets",
          "[unit][sqlite][plan][SQT-001][SQT-015]") {
  const auto empty = snapshot({});
  const auto desired = snapshot(R"sql(
    CREATE TABLE tasks(id INTEGER PRIMARY KEY, title TEXT NOT NULL);
    CREATE INDEX tasks_title_idx ON tasks(title);
    CREATE VIEW task_titles AS SELECT title FROM tasks;
    CREATE TRIGGER tasks_touch AFTER UPDATE ON tasks BEGIN
      SELECT NEW.id;
    END;
  )sql");

  const auto create = dbdiff::sqlite::plan(empty, desired);
  CHECK_FALSE(create.draft);
  CHECK(has_hazard(create, dbdiff::Hazard::write_lock));
  CHECK(create.sql.find("CREATE TABLE tasks") != std::string::npos);
  CHECK(create.sql.find("CREATE INDEX tasks_title_idx") != std::string::npos);
  CHECK(create.sql.find("CREATE VIEW task_titles") != std::string::npos);
  CHECK(create.sql.find("CREATE TRIGGER tasks_touch") != std::string::npos);
  CHECK(dbdiff::sqlite::validate_plan(empty, desired));

  const auto drop = dbdiff::sqlite::plan(desired, empty);
  CHECK(has_hazard(drop, dbdiff::Hazard::data_loss));
  CHECK(has_hazard(drop, dbdiff::Hazard::write_lock));
  CHECK(drop.sql.starts_with("PRAGMA foreign_keys=OFF;\nBEGIN IMMEDIATE;\n"));
  CHECK(drop.sql.find("DROP TRIGGER \"tasks_touch\";") < drop.sql.find("DROP TABLE \"tasks\";"));
  CHECK(drop.sql.find("DROP VIEW \"task_titles\";") < drop.sql.find("DROP TABLE \"tasks\";"));
  CHECK(drop.sql.ends_with("PRAGMA foreign_keys=ON;\n"));
  CHECK(dbdiff::sqlite::validate_plan(desired, empty));
}

TEST_CASE("SQLite planner appends columns without rebuilding a table",
          "[unit][sqlite][plan][SQT-002][MIG-005]") {
  const auto from = snapshot("CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT);");
  const auto to = snapshot(R"sql(
    CREATE TABLE items(
      id INTEGER PRIMARY KEY,
      name TEXT,
      enabled INTEGER NOT NULL DEFAULT 1
    );
  )sql");

  const auto first = dbdiff::sqlite::plan(from, to);
  const auto second = dbdiff::sqlite::plan(from, to);
  CHECK(first == second);
  CHECK_FALSE(first.draft);
  CHECK(has_hazard(first, dbdiff::Hazard::write_lock));
  CHECK_FALSE(has_hazard(first, dbdiff::Hazard::table_rewrite));
  CHECK_FALSE(has_hazard(first, dbdiff::Hazard::data_loss));
  CHECK(first.sql.find("ALTER TABLE \"items\" ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1;") !=
        std::string::npos);
  CHECK(first.sql.starts_with("BEGIN IMMEDIATE;\n"));
  CHECK(first.sql.ends_with("COMMIT;\n"));
  CHECK(first.sql.find("foreign_keys=OFF") == std::string::npos);
  CHECK(dbdiff::sqlite::validate_plan(from, to));
}

TEST_CASE("SQLite planner rebuilds tables deterministically and preserves rows and sequences",
          "[unit][sqlite][plan][SQT-003][SQT-013][MIG-005]") {
  dbdiff::test::TempDirectory directory;
  const auto path = directory.path() / "rebuild.sqlite";
  dbdiff::sqlite::Plan migration;

  {
    auto database =
        dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_write_create);
    database.execute_source(R"sql(
      CREATE TABLE items(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        value TEXT,
        legacy TEXT
      );
      CREATE INDEX items_value_idx ON items(value);
      CREATE VIEW item_values AS SELECT id, value FROM items;
      CREATE TRIGGER items_touch AFTER UPDATE ON items BEGIN
        SELECT NEW.id;
      END;
    )sql");
    database.execute_migration(R"sql(
      BEGIN IMMEDIATE;
      INSERT INTO items(value, legacy) VALUES ('a', 'old-a'), ('b', 'old-b'), ('gone', 'old-c');
      DELETE FROM items WHERE value='gone';
      COMMIT;
    )sql");
    const auto from = database.inspect();

    const auto to = snapshot(R"sql(
      CREATE TABLE items(
        value TEXT NOT NULL DEFAULT '',
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        normalized TEXT GENERATED ALWAYS AS (upper(value)) STORED,
        CHECK(length(value) < 100)
      );
      CREATE INDEX items_value_idx ON items(value);
      CREATE VIEW item_values AS SELECT id, value FROM items;
      CREATE TRIGGER items_touch AFTER UPDATE ON items BEGIN
        SELECT NEW.id;
      END;
    )sql");

    migration = dbdiff::sqlite::plan(from, to);
    CHECK_FALSE(migration.draft);
    CHECK(has_hazard(migration, dbdiff::Hazard::data_loss));
    CHECK(has_hazard(migration, dbdiff::Hazard::table_rewrite));
    CHECK(has_hazard(migration, dbdiff::Hazard::write_lock));
    CHECK_FALSE(has_hazard(migration, dbdiff::Hazard::rowid_reassignment));
    CHECK(migration.sql.starts_with("PRAGMA foreign_keys=OFF;\nBEGIN IMMEDIATE;\n"));
    const auto insert = migration.sql.find("INSERT INTO");
    REQUIRE(insert != std::string::npos);
    const auto insert_end = migration.sql.find('\n', insert);
    const auto insert_sql = migration.sql.substr(insert, insert_end - insert);
    CHECK(insert_sql.find("\"value\",\"id\"") != std::string::npos);
    CHECK(insert_sql.find("normalized") == std::string::npos);
    CHECK(migration.sql.find("UPDATE sqlite_sequence") != std::string::npos);
    CHECK(migration.sql.find("PRAGMA foreign_key_check;\nCOMMIT;\nPRAGMA foreign_keys=ON;") !=
          std::string::npos);
    CHECK(dbdiff::sqlite::validate_plan(from, to));

    database.execute_migration(migration.sql);
    CHECK(database.inspect().semantic_hash == to.semantic_hash);
    database.execute_migration(R"sql(
      BEGIN IMMEDIATE;
      INSERT INTO items(value) VALUES ('d');
      COMMIT;
    )sql");
  }

  CHECK(query_text(path, "SELECT group_concat(id || ':' || value, ',') "
                         "FROM (SELECT id,value FROM items ORDER BY id);") == "1:a,2:b,4:d");
  CHECK(query_text(path, "SELECT seq FROM sqlite_sequence WHERE name='items';") == "4");
  CHECK(query_text(path, "SELECT normalized FROM items WHERE id=1;") == "A");
}

TEST_CASE("SQLite planner preserves accessible rowids and reports inaccessible ones",
          "[unit][sqlite][plan][SQT-013]") {
  const auto from = snapshot("CREATE TABLE data(value TEXT, marker INTEGER);");
  const auto to = snapshot(
      "CREATE TABLE data(marker INTEGER, value TEXT, CHECK(marker IS NULL OR marker>=0));");
  const auto preserved = dbdiff::sqlite::plan(from, to);
  CHECK(has_hazard(preserved, dbdiff::Hazard::table_rewrite));
  CHECK_FALSE(has_hazard(preserved, dbdiff::Hazard::rowid_reassignment));
  CHECK(preserved.sql.find("\"rowid\",\"marker\",\"value\"") != std::string::npos);
  CHECK(preserved.sql.find("SELECT \"rowid\",\"marker\",\"value\"") != std::string::npos);
  CHECK(dbdiff::sqlite::validate_plan(from, to));

  const auto shadowed_from =
      snapshot("CREATE TABLE weird(rowid TEXT, _rowid_ TEXT, oid TEXT, value TEXT);");
  const auto shadowed_to = snapshot(
      "CREATE TABLE weird(value TEXT, rowid TEXT, _rowid_ TEXT, oid TEXT, CHECK(value<>''));");
  const auto reassigned = dbdiff::sqlite::plan(shadowed_from, shadowed_to);
  CHECK(has_hazard(reassigned, dbdiff::Hazard::rowid_reassignment));
  CHECK(dbdiff::sqlite::validate_plan(shadowed_from, shadowed_to));
}

TEST_CASE("SQLite planner marks unknown required-column mappings as drafts",
          "[unit][sqlite][plan][PLN-004]") {
  const auto from = snapshot("CREATE TABLE records(id INTEGER PRIMARY KEY);");
  const auto to = snapshot("CREATE TABLE records(id INTEGER PRIMARY KEY, payload TEXT NOT NULL);");
  const auto migration = dbdiff::sqlite::plan(from, to);

  CHECK(migration.draft);
  CHECK(migration.sql.starts_with("-- dbdiff:draft\nPRAGMA foreign_keys=OFF;"));
  CHECK(has_hazard(migration, dbdiff::Hazard::table_rewrite));
  CHECK(dbdiff::sqlite::validate_plan(from, to));
}

TEST_CASE("SQLite planner quotes rebuild identifiers", "[unit][sqlite][plan]") {
  const auto from = snapshot(R"sql(
    CREATE TABLE "odd table"("select" TEXT, "say""hi" INTEGER);
  )sql");
  const auto to = snapshot(R"sql(
    CREATE TABLE "odd table"(
      "say""hi" INTEGER,
      "select" TEXT,
      CHECK("say""hi" IS NULL OR "say""hi" >= 0)
    );
  )sql");

  const auto migration = dbdiff::sqlite::plan(from, to);
  CHECK(migration.sql.find("DROP TABLE \"odd table\";") != std::string::npos);
  CHECK(migration.sql.find("\"say\"\"hi\"") != std::string::npos);
  CHECK(migration.sql.find("FROM \"odd table\";") != std::string::npos);
  CHECK(dbdiff::sqlite::validate_plan(from, to));
}

TEST_CASE("SQLite planner replaces changed indexes views and triggers",
          "[unit][sqlite][plan][SQT-014][SQT-015]") {
  const auto from = snapshot(R"sql(
    CREATE TABLE values_table(id INTEGER PRIMARY KEY, value TEXT);
    CREATE INDEX values_idx ON values_table(value);
    CREATE VIEW values_view AS SELECT value FROM values_table;
    CREATE TRIGGER values_trigger AFTER INSERT ON values_table BEGIN SELECT NEW.id; END;
  )sql");
  const auto to = snapshot(R"sql(
    CREATE TABLE values_table(id INTEGER PRIMARY KEY, value TEXT);
    CREATE UNIQUE INDEX values_idx ON values_table(value DESC);
    CREATE VIEW values_view AS SELECT id, value FROM values_table;
    CREATE TRIGGER values_trigger AFTER UPDATE ON values_table BEGIN SELECT NEW.id; END;
  )sql");

  const auto migration = dbdiff::sqlite::plan(from, to);
  CHECK(migration.sql.find("DROP INDEX \"values_idx\";") != std::string::npos);
  CHECK(migration.sql.find("DROP VIEW \"values_view\";") != std::string::npos);
  CHECK(migration.sql.find("DROP TRIGGER \"values_trigger\";") != std::string::npos);
  CHECK(migration.sql.find("CREATE UNIQUE INDEX values_idx") != std::string::npos);
  CHECK(migration.sql.find("CREATE VIEW values_view") != std::string::npos);
  CHECK(migration.sql.find("CREATE TRIGGER values_trigger") != std::string::npos);
  CHECK(dbdiff::sqlite::validate_plan(from, to));
}
