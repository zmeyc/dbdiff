#include "dbdiff/error.hpp"
#include "dbdiff/sqlite.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace {

const dbdiff::sqlite::TableSnapshot& table_named(const dbdiff::sqlite::SchemaSnapshot& schema,
                                                 const std::string_view name) {
  const auto table = std::ranges::find(schema.tables, name, &dbdiff::sqlite::TableSnapshot::name);
  REQUIRE(table != schema.tables.end());
  return *table;
}
const dbdiff::sqlite::IndexSnapshot& index_named(const dbdiff::sqlite::SchemaSnapshot& schema,
                                                 const std::string_view name) {
  const auto index = std::ranges::find(schema.indexes, name, &dbdiff::sqlite::IndexSnapshot::name);
  REQUIRE(index != schema.indexes.end());
  return *index;
}

} // namespace

TEST_CASE("SQLite statement scanning preserves trigger bodies and transaction boundaries",
          "[unit][sqlite]") {
  const std::string sql = R"sql(
    CREATE TABLE messages(id INTEGER PRIMARY KEY, body TEXT);
    CREATE TRIGGER messages_audit AFTER INSERT ON messages BEGIN
      UPDATE messages SET body = body || '; audited' WHERE id = NEW.id;
      SELECT CASE WHEN NEW.body = '/* not a comment; */' THEN 1 END;
    END;
    BEGIN IMMEDIATE;
    INSERT INTO messages(body) VALUES ('value; still one statement');
    COMMIT
  )sql";

  const auto statements = dbdiff::sqlite::scan_statements(sql);
  REQUIRE(statements.size() == 5U);
  CHECK(statements[0].kind == dbdiff::StatementKind::ddl);
  CHECK(statements[1].kind == dbdiff::StatementKind::ddl);
  CHECK(statements[2].kind == dbdiff::StatementKind::begin);
  CHECK(statements[3].kind == dbdiff::StatementKind::dml);
  CHECK(statements[4].kind == dbdiff::StatementKind::commit);
  CHECK(std::string_view{sql}
            .substr(statements[1].begin, statements[1].end - statements[1].begin)
            .find("UPDATE messages") != std::string_view::npos);
}

TEST_CASE("SQLite inspection captures supported schema properties", "[unit][sqlite]") {
  auto database = dbdiff::sqlite::Database::temporary();
  database.execute_source(R"sql(
    CREATE TABLE parent(
      id INTEGER PRIMARY KEY,
      name TEXT NOT NULL UNIQUE
    );
    CREATE TABLE child(
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      parent_id INTEGER NOT NULL REFERENCES parent(id) ON DELETE CASCADE,
      note TEXT,
      normalized TEXT GENERATED ALWAYS AS (lower(coalesce(note, ''))) STORED
    ) STRICT;
    CREATE INDEX child_note_idx
      ON child(lower(note) COLLATE NOCASE DESC)
      WHERE note IS NOT NULL;
    CREATE VIEW parent_names AS SELECT id, name FROM parent;
    CREATE TRIGGER child_touch AFTER INSERT ON child BEGIN
      UPDATE parent SET name = name WHERE id = NEW.parent_id;
    END;
  )sql");

  const auto schema = database.inspect();
  REQUIRE(schema.tables.size() == 2U);
  const auto& child = table_named(schema, "child");
  CHECK(child.strict);
  CHECK_FALSE(child.without_rowid);
  REQUIRE(child.columns.size() == 4U);
  CHECK(child.columns[3].generated == dbdiff::sqlite::GeneratedColumnKind::stored_generated);
  REQUIRE(child.foreign_keys.size() == 1U);
  CHECK(child.foreign_keys[0].parent_table == "parent");
  CHECK(child.foreign_keys[0].on_delete == "CASCADE");
  REQUIRE(child.foreign_keys[0].columns.size() == 1U);
  CHECK(child.foreign_keys[0].columns[0].from_column == "parent_id");

  const auto& index = index_named(schema, "child_note_idx");
  CHECK(index.partial);
  CHECK(index.origin == "c");
  REQUIRE(index.columns.size() >= 1U);
  CHECK(index.columns[0].table_column_rank == -2);
  CHECK(index.columns[0].descending);
  REQUIRE(index.columns[0].collation.has_value());
  CHECK(index.columns[0].collation.value_or("") == "NOCASE");

  REQUIRE(schema.objects.size() == 2U);
  CHECK(schema.semantic_hash.size() == 64U);
}

TEST_CASE("SQLite semantic hashes ignore schema formatting and identifier quoting",
          "[unit][sqlite]") {
  auto first = dbdiff::sqlite::Database::temporary();
  first.execute_source("CREATE TABLE users(id INTEGER PRIMARY KEY, email TEXT DEFAULT 'unknown');");

  auto second = dbdiff::sqlite::Database::temporary();
  second.execute_source(R"sql(
    -- The same declaration with different lexical formatting.
    create table [users]
    (
      [id] integer primary key,
      [email] text default 'unknown'
    );
  )sql");

  CHECK(first.inspect().semantic_hash == second.inspect().semantic_hash);

  auto changed = dbdiff::sqlite::Database::temporary();
  changed.execute_source(
      "CREATE TABLE users(id INTEGER PRIMARY KEY, email TEXT NOT NULL DEFAULT 'unknown');");
  CHECK(first.inspect().semantic_hash != changed.inspect().semantic_hash);
}

TEST_CASE("SQLite migration execution honors visible transactions and DML placement",
          "[unit][sqlite]") {
  auto database = dbdiff::sqlite::Database::temporary();
  database.execute_source("CREATE TABLE entries(id INTEGER PRIMARY KEY, value TEXT);");

  database.execute_migration(R"sql(
    BEGIN IMMEDIATE;
    INSERT INTO entries(id, value) VALUES (1, 'one');
    ALTER TABLE entries ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1;
    CREATE INDEX entries_value_idx ON entries(value);
    COMMIT;
  )sql");

  const auto schema = database.inspect();
  REQUIRE(table_named(schema, "entries").columns.size() == 3U);
  CHECK(index_named(schema, "entries_value_idx").table == "entries");

  CHECK_THROWS_AS(database.execute_migration("INSERT INTO entries(value) VALUES ('two');"),
                  dbdiff::Error);
  CHECK_THROWS_AS(database.execute_migration("BEGIN; CREATE TABLE unfinished(id);"), dbdiff::Error);
  CHECK_THROWS_AS(database.execute_migration("PRAGMA journal_mode=WAL;"), dbdiff::Error);
}

TEST_CASE("SQLite foreign-key validation rolls back a failed migration", "[unit][sqlite]") {
  auto database = dbdiff::sqlite::Database::temporary();
  database.execute_source(R"sql(
    CREATE TABLE parent(id INTEGER PRIMARY KEY);
    CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id));
  )sql");

  CHECK_THROWS_AS(database.execute_migration(R"sql(
    PRAGMA foreign_keys=OFF;
    BEGIN IMMEDIATE;
    INSERT INTO child(id, parent_id) VALUES (99, 99);
    PRAGMA foreign_key_check;
    COMMIT;
    PRAGMA foreign_keys=ON;
  )sql"),
                  dbdiff::Error);

  CHECK_NOTHROW(database.execute_migration(R"sql(
    BEGIN IMMEDIATE;
    INSERT INTO parent(id) VALUES (1);
    INSERT INTO child(id, parent_id) VALUES (1, 1);
    PRAGMA foreign_key_check;
    COMMIT;
  )sql"));
}

TEST_CASE("SQLite declarative source execution rejects non-schema behavior", "[unit][sqlite]") {
  auto database = dbdiff::sqlite::Database::temporary();
  CHECK_THROWS_AS(
      database.execute_source("CREATE TABLE data(id INTEGER); INSERT INTO data VALUES (1);"),
      dbdiff::Error);
  CHECK_THROWS_AS(database.execute_source("CREATE TABLE copied AS SELECT 1 AS value;"),
                  dbdiff::Error);
  CHECK_THROWS_AS(database.execute_source("CREATE TEMP TABLE transient(id INTEGER);"),
                  dbdiff::Error);
  CHECK_THROWS_AS(database.execute_source("CREATE VIRTUAL TABLE search USING fts5(contents);"),
                  dbdiff::Error);
  CHECK(database.inspect().tables.empty());
}

TEST_CASE("SQLite databases can be reopened read-only for inspection", "[unit][sqlite]") {
  dbdiff::test::TempDirectory directory;
  const auto path = directory.path() / "schema.sqlite";
  {
    auto database =
        dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_write_create);
    database.execute_source("CREATE TABLE persistent(id INTEGER PRIMARY KEY);");
  }

  auto database = dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_only);
  REQUIRE(database.inspect().tables.size() == 1U);
  CHECK(table_named(database.inspect(), "persistent").name == "persistent");
  CHECK_THROWS_AS(database.execute_migration("CREATE TABLE forbidden(id INTEGER);"), dbdiff::Error);
}
