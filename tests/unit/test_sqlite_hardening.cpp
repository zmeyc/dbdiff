#include "dbdiff/error.hpp"
#include "dbdiff/sqlite.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

class RawDatabase final {
public:
  explicit RawDatabase(const std::filesystem::path& path) {
    const auto result = sqlite3_open_v2(
        path.string().c_str(), &handle_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
      const auto message = handle_ == nullptr ? std::string{"cannot open SQLite test database"}
                                              : std::string{sqlite3_errmsg(handle_)};
      if (handle_ != nullptr) {
        static_cast<void>(sqlite3_close_v2(handle_));
        handle_ = nullptr;
      }
      throw std::runtime_error{message};
    }
  }

  ~RawDatabase() {
    if (handle_ != nullptr) {
      static_cast<void>(sqlite3_close_v2(handle_));
    }
  }

  RawDatabase(const RawDatabase&) = delete;
  RawDatabase& operator=(const RawDatabase&) = delete;

  void execute(const std::string_view sql) {
    char* message = nullptr;
    const auto result = sqlite3_exec(handle_, std::string{sql}.c_str(), nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
      const auto detail =
          message == nullptr ? std::string{sqlite3_errmsg(handle_)} : std::string{message};
      sqlite3_free(message);
      throw std::runtime_error{detail};
    }
  }

private:
  sqlite3* handle_{nullptr};
};

const dbdiff::sqlite::TableSnapshot& table_named(const dbdiff::sqlite::SchemaSnapshot& schema,
                                                 const std::string_view name) {
  const auto found = std::ranges::find(schema.tables, name, &dbdiff::sqlite::TableSnapshot::name);
  REQUIRE(found != schema.tables.end());
  return *found;
}

void require_writer_waits_safely(const std::filesystem::path& path,
                                 const std::string_view journal_mode,
                                 const std::string_view created_table) {
  RawDatabase holder{path};
  holder.execute("PRAGMA journal_mode=" + std::string{journal_mode} + ";");
  holder.execute("CREATE TABLE seed(id INTEGER PRIMARY KEY, value TEXT);");
  holder.execute("BEGIN IMMEDIATE;");
  holder.execute("INSERT INTO seed(id,value) VALUES(1,'held writer');");

  std::jthread release_writer{[&holder] {
    std::this_thread::sleep_for(100ms);
    holder.execute("COMMIT;");
  }};

  auto contender = dbdiff::sqlite::Database::open(path, dbdiff::sqlite::OpenMode::read_write);
  CHECK_NOTHROW(contender.execute_source("CREATE TABLE " + std::string{created_table} +
                                         "(id INTEGER PRIMARY KEY);"));
  release_writer.join();
  CHECK(table_named(contender.inspect(), created_table).name == created_table);
}

} // namespace

TEST_CASE("SQLite rich schemas survive semantic render and reconstruction",
          "[unit][sqlite][SQT-010][SQT-011][SQT-012][SQT-014][SQT-015]") {
  auto original = dbdiff::sqlite::Database::temporary();
  original.execute_source(R"sql(
    CREATE TABLE parent(
      tenant INTEGER,
      code TEXT COLLATE NOCASE,
      PRIMARY KEY(tenant, code)
    ) WITHOUT ROWID, STRICT;
    CREATE TABLE audit(
      id INTEGER PRIMARY KEY,
      detail TEXT NOT NULL
    ) STRICT;
    CREATE TABLE rich(
      id INTEGER PRIMARY KEY,
      code VARCHAR(20) COLLATE NOCASE NOT NULL DEFAULT ('unknown'),
      quantity NUMERIC DEFAULT (+0),
      normalized TEXT GENERATED ALWAYS AS (lower(code)) VIRTUAL,
      doubled INTEGER GENERATED ALWAYS AS (quantity * 2) STORED,
      parent_tenant INTEGER,
      parent_code TEXT,
      CONSTRAINT rich_pair UNIQUE(parent_tenant, parent_code),
      CONSTRAINT rich_quantity CHECK(quantity >= 0),
      CONSTRAINT rich_parent FOREIGN KEY(parent_tenant, parent_code)
        REFERENCES parent(tenant, code)
        ON UPDATE CASCADE ON DELETE SET NULL DEFERRABLE INITIALLY DEFERRED
    );
    CREATE UNIQUE INDEX rich_expression_idx
      ON rich(lower(code) COLLATE NOCASE DESC, quantity)
      WHERE quantity IS NOT NULL;
    CREATE VIEW rich_view AS
      SELECT id, normalized, doubled FROM rich WHERE quantity >= 0;
    CREATE TRIGGER rich_audit AFTER INSERT ON rich BEGIN
      UPDATE rich SET quantity = coalesce(quantity, 0) WHERE id = NEW.id;
      INSERT INTO audit(detail) VALUES ('created:' || NEW.id);
    END;
  )sql");

  const auto expected = original.inspect();
  CHECK_FALSE(table_named(expected, "rich").strict);
  CHECK(table_named(expected, "parent").strict);
  CHECK(table_named(expected, "parent").without_rowid);
  REQUIRE(table_named(expected, "rich").columns.size() == 7U);
  CHECK(table_named(expected, "rich").columns[3].generated ==
        dbdiff::sqlite::GeneratedColumnKind::virtual_generated);
  CHECK(table_named(expected, "rich").columns[4].generated ==
        dbdiff::sqlite::GeneratedColumnKind::stored_generated);

  const auto rendered = dbdiff::sqlite::render_snapshot(expected);
  auto reconstructed = dbdiff::sqlite::Database::temporary();
  reconstructed.execute_migration(rendered);
  CHECK(reconstructed.inspect().semantic_hash == expected.semantic_hash);

  const auto empty = dbdiff::sqlite::Database::temporary().inspect();
  CHECK(dbdiff::sqlite::validate_plan(empty, expected));
}

TEST_CASE("SQLite rejects unsafe schema scopes before committed work can escape",
          "[unit][sqlite][SQT-004]") {
  auto database = dbdiff::sqlite::Database::temporary();

  CHECK_THROWS_AS(database.execute_migration(R"sql(
    BEGIN IMMEDIATE;
    CREATE TABLE persistent_leak(id INTEGER);
    COMMIT;
    CREATE TABLE temp.transient_leak(id INTEGER);
  )sql"),
                  dbdiff::Error);
  CHECK(database.inspect().tables.empty());

  CHECK_THROWS_AS(database.execute_migration(R"sql(
    BEGIN IMMEDIATE;
    CREATE TABLE copied_leak(id INTEGER);
    COMMIT;
    CREATE TABLE unsafe_copy AS SELECT 1 AS id;
  )sql"),
                  dbdiff::Error);
  CHECK(database.inspect().tables.empty());

  CHECK_THROWS_AS(database.execute_migration(
                      "ATTACH DATABASE ':memory:' AS auxiliary; CREATE TABLE auxiliary.item(id);"),
                  dbdiff::Error);
  CHECK(database.inspect().tables.empty());
}

TEST_CASE("SQLite rejects virtual and unrecognized reserved target objects",
          "[unit][sqlite][SQT-004]") {
  dbdiff::test::TempDirectory directory;

  const auto reserved_path = directory.path() / "reserved.sqlite";
  {
    RawDatabase raw{reserved_path};
    raw.execute("CREATE TABLE _dbdiff_unrecognized(id INTEGER);");
  }
  auto reserved =
      dbdiff::sqlite::Database::open(reserved_path, dbdiff::sqlite::OpenMode::read_only);
  CHECK_THROWS_AS(reserved.inspect(), dbdiff::Error);

  if (sqlite3_compileoption_used("ENABLE_FTS5") == 0) {
    return;
  }
  const auto virtual_path = directory.path() / "virtual.sqlite";
  {
    RawDatabase raw{virtual_path};
    raw.execute("CREATE VIRTUAL TABLE documents USING fts5(body);");
  }
  auto virtual_database =
      dbdiff::sqlite::Database::open(virtual_path, dbdiff::sqlite::OpenMode::read_only);
  CHECK_THROWS_AS(virtual_database.inspect(), dbdiff::Error);
}

TEST_CASE("SQLite opens only plain persistent non-symlink paths", "[unit][sqlite][SQT-004]") {
  CHECK_THROWS_AS(dbdiff::sqlite::Database::open("file:unsafe?mode=memory",
                                                 dbdiff::sqlite::OpenMode::read_write_create),
                  dbdiff::Error);
  CHECK_THROWS_AS(
      dbdiff::sqlite::Database::open(":memory:", dbdiff::sqlite::OpenMode::read_write_create),
      dbdiff::Error);

  dbdiff::test::TempDirectory directory;
  const auto target = directory.path() / "target.sqlite";
  {
    auto database =
        dbdiff::sqlite::Database::open(target, dbdiff::sqlite::OpenMode::read_write_create);
    database.execute_source("CREATE TABLE safe(id INTEGER PRIMARY KEY);");
  }
  const auto link = directory.path() / "target-link.sqlite";
  std::filesystem::create_symlink(target, link);
  CHECK_THROWS_AS(dbdiff::sqlite::Database::open(link, dbdiff::sqlite::OpenMode::read_only),
                  dbdiff::Error);
}

TEST_CASE("SQLite busy handling serializes rollback-journal and WAL writers",
          "[unit][sqlite][SQT-005][OPS-002]") {
  dbdiff::test::TempDirectory directory;
  require_writer_waits_safely(directory.path() / "rollback.sqlite", "DELETE", "after_rollback");
  require_writer_waits_safely(directory.path() / "wal.sqlite", "WAL", "after_wal");
}
