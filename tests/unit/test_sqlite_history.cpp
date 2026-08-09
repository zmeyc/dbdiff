#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"
#include "dbdiff/sqlite.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] bool has_table(const dbdiff::sqlite::SchemaSnapshot& schema,
                             const std::string_view name) {
  return std::ranges::any_of(schema.tables,
                             [name](const auto& table) { return table.name == name; });
}

} // namespace

TEST_CASE("SQLite history reads do not create metadata", "[unit][sqlite][history]") {
  auto memory = dbdiff::sqlite::Database::temporary();
  const auto empty = memory.read_history();
  CHECK_FALSE(empty.initialized);
  CHECK(empty.entries.empty());
  CHECK(memory.inspect().tables.empty());

  dbdiff::test::TempDirectory directory;
  const auto path = directory.path() / "readonly.sqlite";
  {
    auto writable =
        dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_write_create);
    writable.execute_source("CREATE TABLE application_data(id INTEGER PRIMARY KEY);");
  }
  auto readonly = dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_only);
  CHECK_FALSE(readonly.read_history().initialized);
  CHECK(has_table(readonly.inspect(), "application_data"));
}

TEST_CASE("SQLite failed migrations preserve an immutable prefix and exact revisions",
          "[unit][sqlite][history]") {
  auto database = dbdiff::sqlite::Database::temporary();
  const std::string prefix = R"sql(BEGIN IMMEDIATE;
CREATE TABLE accounts(id INTEGER PRIMARY KEY, email TEXT NOT NULL);
COMMIT;)sql";
  const auto failed = prefix + R"sql(
BEGIN IMMEDIATE;
CREATE TABLE audit(id INTEGER PRIMARY KEY);
INSERT INTO table_that_does_not_exist(id) VALUES (1);
COMMIT;
)sql";
  const auto failed_sha = dbdiff::sha256_hex(failed);

  CHECK_THROWS_AS(database.apply_version("20260808010101_accounts", failed_sha, failed, false),
                  dbdiff::Error);

  auto history = database.read_history();
  REQUIRE(history.initialized);
  REQUIRE(history.entries.size() == 1U);
  CHECK_FALSE(history.entries[0].completed_file_sha256.has_value());
  REQUIRE(history.entries[0].units.size() == 1U);
  CHECK(history.entries[0].units[0].ordinal == 0U);
  CHECK(history.entries[0].units[0].explicit_transaction);
  CHECK(history.entries[0].units[0].state == dbdiff::sqlite::MigrationUnitState::completed);
  CHECK(has_table(database.inspect(), "accounts"));
  CHECK_FALSE(has_table(database.inspect(), "audit"));

  const auto changed_prefix = std::string{R"sql(BEGIN IMMEDIATE;
CREATE TABLE accounts(id INTEGER PRIMARY KEY, email TEXT);
COMMIT;)sql"} +
                              R"sql(
BEGIN IMMEDIATE;
CREATE TABLE audit(id INTEGER PRIMARY KEY);
COMMIT;
)sql";
  CHECK_THROWS_AS(database.apply_version("20260808010101_accounts",
                                         dbdiff::sha256_hex(changed_prefix), changed_prefix, true),
                  dbdiff::Error);
  CHECK_FALSE(has_table(database.inspect(), "audit"));

  const auto repaired = prefix + R"sql(
BEGIN IMMEDIATE;
CREATE TABLE audit(id INTEGER PRIMARY KEY, account_id INTEGER REFERENCES accounts(id));
INSERT INTO accounts(id, email) VALUES (1, 'first@example.test');
COMMIT;
)sql";
  const auto repaired_sha = dbdiff::sha256_hex(repaired);
  const auto applied =
      database.apply_version("20260808010101_accounts", repaired_sha, repaired, true);
  CHECK_FALSE(applied.already_completed);
  CHECK(applied.completed_unit_count == 2U);
  CHECK(applied.completed_file_sha256 == repaired_sha);
  const auto application_schema = database.inspect();
  CHECK(application_schema.tables.size() == 2U);
  CHECK(has_table(application_schema, "audit"));

  history = database.read_history();
  REQUIRE(history.entries.size() == 1U);
  CHECK(history.entries[0].backend == "sqlite");
  CHECK_FALSE(history.entries[0].engine_version.empty());
  CHECK(history.entries[0].attempted_file_sha256 == repaired_sha);
  REQUIRE(history.entries[0].completed_file_sha256.has_value());
  CHECK(history.entries[0].completed_file_sha256.value_or("") == repaired_sha);
  REQUIRE(history.entries[0].units.size() == 2U);

  const auto recovered = database.recover_revisions("20260808010101_accounts");
  REQUIRE(recovered.size() == 3U);
  CHECK(recovered[0].ordinal == 0U);
  CHECK(recovered[0].exact_file_sha256 == failed_sha);
  CHECK(recovered[0].sql == failed);
  CHECK(recovered[1].sql == changed_prefix);
  CHECK(recovered[2].exact_file_sha256 == repaired_sha);
  CHECK(recovered[2].sql == repaired);

  const auto repeated =
      database.apply_version("20260808010101_accounts", repaired_sha, repaired, false);
  CHECK(repeated.already_completed);
  CHECK(repeated.completed_unit_count == 2U);
  CHECK_THROWS_AS(database.apply_version("20260808010101_accounts", failed_sha, failed, false),
                  dbdiff::Error);
}

TEST_CASE("SQLite migration checksums cover every exact file byte", "[unit][sqlite][history]") {
  auto database = dbdiff::sqlite::Database::temporary();
  const std::string sql{"BEGIN;\nCREATE TABLE exact_bytes(id INTEGER);\nCOMMIT;\n"};
  auto wrong = dbdiff::sha256_hex(sql);
  wrong[0] = wrong[0] == '0' ? '1' : '0';

  CHECK_THROWS_AS(database.apply_version("20260808020202_exact", wrong, sql, false), dbdiff::Error);
  CHECK_FALSE(database.read_history().initialized);

  const auto hash = dbdiff::sha256_hex(sql);
  CHECK_NOTHROW(database.apply_version("20260808020202_exact", hash, sql, false));
  const auto recovered = database.recover_revisions("20260808020202_exact");
  REQUIRE(recovered.size() == 1U);
  CHECK(recovered[0].sql == sql);
  CHECK(recovered[0].exact_file_sha256 == hash);
}

TEST_CASE("SQLite history rejects invalid or incompatible engine metadata",
          "[unit][sqlite][history][MIG-006]") {
  dbdiff::test::TempDirectory directory;
  const auto path = directory.path() / "engine.sqlite";
  const std::string failed{R"sql(BEGIN;
CREATE TABLE first(id INTEGER);
COMMIT;
BEGIN;
CREATE TABLE first(id INTEGER);
COMMIT;
)sql"};
  const std::string repaired{R"sql(BEGIN;
CREATE TABLE first(id INTEGER);
COMMIT;
BEGIN;
CREATE TABLE second(id INTEGER);
COMMIT;
)sql"};
  {
    auto database =
        dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_write_create);
    CHECK_THROWS_AS(
        database.apply_version("20260808020203_engine", dbdiff::sha256_hex(failed), failed, false),
        dbdiff::Error);
  }

  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
  const auto close_raw = [](sqlite3* database) { static_cast<void>(sqlite3_close_v2(database)); };
  const std::unique_ptr<sqlite3, decltype(close_raw)> connection{raw, close_raw};
  REQUIRE(sqlite3_exec(connection.get(),
                       "UPDATE _dbdiff_migrations SET engine_version='3.999.999';", nullptr,
                       nullptr, nullptr) == SQLITE_OK);

  {
    auto database = dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_write);
    CHECK_THROWS_AS(database.apply_version("20260808020203_engine", dbdiff::sha256_hex(repaired),
                                           repaired, true),
                    dbdiff::Error);
  }

  REQUIRE(sqlite3_exec(connection.get(),
                       "UPDATE _dbdiff_migrations SET engine_version='not-a-version';", nullptr,
                       nullptr, nullptr) == SQLITE_OK);
  {
    auto database = dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_only);
    CHECK_THROWS_AS(database.read_history(), dbdiff::Error);
  }
}

TEST_CASE("SQLite standalone DDL can resume from an edited incomplete suffix",
          "[unit][sqlite][history]") {
  auto database = dbdiff::sqlite::Database::temporary();
  const std::string failed{"CREATE TABLE first(id INTEGER);\n"
                           "CREATE TABLE first(id INTEGER);\n"};
  CHECK_THROWS_AS(database.apply_version("20260808030303_standalone", dbdiff::sha256_hex(failed),
                                         failed, false),
                  dbdiff::Error);
  REQUIRE(database.read_history().entries[0].units.size() == 1U);
  CHECK_FALSE(database.read_history().entries[0].units[0].explicit_transaction);

  const std::string repaired{"CREATE TABLE first(id INTEGER);\n"
                             "CREATE TABLE second(id INTEGER);\n"};
  CHECK_NOTHROW(database.apply_version("20260808030303_standalone", dbdiff::sha256_hex(repaired),
                                       repaired, true));
  CHECK(has_table(database.inspect(), "first"));
  CHECK(has_table(database.inspect(), "second"));
}

TEST_CASE("SQLite completed session directives are replayed during resume",
          "[unit][sqlite][history]") {
  auto database = dbdiff::sqlite::Database::temporary();
  database.execute_source(R"sql(
    CREATE TABLE parent(id INTEGER PRIMARY KEY);
    CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id));
  )sql");
  const std::string prefix{"PRAGMA foreign_keys=OFF;"};
  const auto failed = prefix + R"sql(
BEGIN IMMEDIATE;
INSERT INTO child(id, parent_id) VALUES (1, 99);
INSERT INTO missing_table(id) VALUES (1);
COMMIT;
)sql";
  CHECK_THROWS_AS(
      database.apply_version("20260808040404_session", dbdiff::sha256_hex(failed), failed, false),
      dbdiff::Error);

  const auto repaired = prefix + R"sql(
BEGIN IMMEDIATE;
INSERT INTO child(id, parent_id) VALUES (1, 99);
COMMIT;
PRAGMA foreign_keys=ON;
)sql";
  CHECK_NOTHROW(database.apply_version("20260808040404_session", dbdiff::sha256_hex(repaired),
                                       repaired, true));
  CHECK_THROWS_AS(database.execute_migration("PRAGMA foreign_key_check;"), dbdiff::Error);
}

TEST_CASE("SQLite scratch reconstruction executes an exact unit prefix without history",
          "[unit][sqlite][history]") {
  auto database = dbdiff::sqlite::Database::temporary();
  const std::string sql{R"sql(BEGIN;
CREATE TABLE first(id INTEGER);
COMMIT;
BEGIN;
CREATE TABLE second(id INTEGER);
COMMIT;
)sql"};

  database.execute_prefix(sql, 1U);
  CHECK(has_table(database.inspect(), "first"));
  CHECK_FALSE(has_table(database.inspect(), "second"));
  CHECK_FALSE(database.read_history().initialized);
  CHECK_THROWS_AS(database.execute_prefix(sql, 3U), dbdiff::Error);
}

TEST_CASE("SQLite online backup copies committed schema and WAL data", "[unit][sqlite][history]") {
  dbdiff::test::TempDirectory directory;
  const auto path = directory.path() / "source.sqlite";
  sqlite3* raw_writer = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &raw_writer, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          nullptr) == SQLITE_OK);
  const auto close_writer = [](sqlite3* database) {
    static_cast<void>(sqlite3_close_v2(database));
  };
  const std::unique_ptr<sqlite3, decltype(close_writer)> writer{raw_writer, close_writer};
  REQUIRE(sqlite3_exec(writer.get(),
                       "PRAGMA journal_mode=WAL;"
                       "PRAGMA wal_autocheckpoint=0;"
                       "CREATE TABLE wal_items(id INTEGER PRIMARY KEY,value TEXT NOT NULL);"
                       "INSERT INTO wal_items(id,value) VALUES(1,'committed in WAL');",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(std::filesystem::exists(path.string() + "-wal"));

  auto source = dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_only);
  auto destination = dbdiff::sqlite::Database::temporary();
  source.backup_to(destination);

  CHECK(has_table(destination.inspect(), "wal_items"));
  CHECK_THROWS_AS(destination.execute_migration(R"sql(
    BEGIN;
    INSERT INTO wal_items(id,value) VALUES(1,'duplicate');
    COMMIT;
  )sql"),
                  dbdiff::Error);
  CHECK_NOTHROW(destination.execute_migration(R"sql(
    BEGIN;
    INSERT INTO wal_items(id,value) VALUES(2,'copied database remains writable');
    COMMIT;
  )sql"));
}
