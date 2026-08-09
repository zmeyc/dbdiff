#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"
#include "dbdiff/postgresql.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace pg = dbdiff::postgresql;

namespace {

[[nodiscard]] dbdiff::ParsedScript migration(const std::string& suffix = {}) {
  return pg::parse_migration(
      "BEGIN;\nCREATE TABLE public.items(id bigint);\nCOMMIT;\n" +
      (suffix.empty() ? std::string{"CREATE INDEX items_id_idx ON public.items(id);\n"} : suffix));
}

[[nodiscard]] pg::MigrationUnitRecord completed_unit(const dbdiff::ExecutionUnit& unit) {
  return pg::MigrationUnitRecord{
      .ordinal = unit.ordinal,
      .exact_sha256 = unit.exact_sha256,
      .explicit_transaction = unit.explicit_transaction,
      .before_schema_sha256 = std::string(64U, 'a'),
      .after_schema_sha256 = std::string(64U, 'b'),
      .state = pg::MigrationUnitState::completed,
  };
}

[[nodiscard]] pg::MigrationHistoryEntry incomplete_history(const dbdiff::ParsedScript& parsed) {
  return pg::MigrationHistoryEntry{
      .version = "20260808010101_history",
      .backend = "postgresql",
      .engine_version = "15",
      .attempted_file_sha256 = dbdiff::sha256_hex(parsed.sql),
      .completed_file_sha256 = std::nullopt,
      .units = {completed_unit(parsed.units[0])},
  };
}

} // namespace

static_assert(std::is_same_v<decltype(&pg::Database::read_history),
                             pg::MigrationHistory (pg::Database::*)() const>);
static_assert(
    std::is_same_v<decltype(&pg::Database::recover_revisions),
                   std::vector<pg::MigrationRevision> (pg::Database::*)(std::string_view) const>);

TEST_CASE("PostgreSQL migration history starts or resumes only explicitly",
          "[unit][postgresql][history][MIG-002][MIG-003]") {
  const auto parsed = migration();
  const auto exact_file_sha256 = dbdiff::sha256_hex(parsed.sql);

  const auto initial =
      pg::validate_migration_resume(std::nullopt, parsed, exact_file_sha256, false);
  CHECK_FALSE(initial.already_completed);
  CHECK(initial.completed_unit_count == 0U);
  CHECK(initial.completed_file_sha256.empty());
  CHECK_THROWS_AS(pg::validate_migration_resume(std::nullopt, parsed, exact_file_sha256, true),
                  dbdiff::Error);

  const auto history = incomplete_history(parsed);
  CHECK_THROWS_AS(pg::validate_migration_resume(history, parsed, exact_file_sha256, false),
                  dbdiff::Error);
  const auto resumed = pg::validate_migration_resume(history, parsed, exact_file_sha256, true);
  CHECK_FALSE(resumed.already_completed);
  CHECK(resumed.completed_unit_count == 1U);
}

TEST_CASE("PostgreSQL completed unit bytes and boundaries are immutable",
          "[unit][postgresql][history][MIG-003]") {
  const auto original = migration();
  const auto history = incomplete_history(original);

  const auto edited_suffix = migration("CREATE INDEX changed_idx ON public.items(id);\n");
  const auto resumed = pg::validate_migration_resume(history, edited_suffix,
                                                     dbdiff::sha256_hex(edited_suffix.sql), true);
  CHECK(resumed.completed_unit_count == 1U);

  const auto edited_prefix =
      pg::parse_migration("BEGIN;\nCREATE  TABLE public.items(id bigint);\nCOMMIT;\n"
                          "CREATE INDEX changed_idx ON public.items(id);\n");
  CHECK_THROWS_AS(pg::validate_migration_resume(history, edited_prefix,
                                                dbdiff::sha256_hex(edited_prefix.sql), true),
                  dbdiff::Error);

  auto changed_boundary = history;
  changed_boundary.units[0].explicit_transaction = false;
  CHECK_THROWS_AS(pg::validate_migration_resume(changed_boundary, original,
                                                dbdiff::sha256_hex(original.sql), true),
                  dbdiff::Error);
}

TEST_CASE("PostgreSQL completed migration files are exact and idempotent",
          "[unit][postgresql][history][MIG-003]") {
  const auto parsed = migration();
  const auto exact_file_sha256 = dbdiff::sha256_hex(parsed.sql);
  auto history = incomplete_history(parsed);
  history.units.push_back(completed_unit(parsed.units[1]));
  history.completed_file_sha256 = exact_file_sha256;

  const auto completed = pg::validate_migration_resume(history, parsed, exact_file_sha256, false);
  CHECK(completed.already_completed);
  CHECK(completed.completed_unit_count == parsed.units.size());
  CHECK(completed.completed_file_sha256 == exact_file_sha256);

  const auto edited = migration("CREATE INDEX another_idx ON public.items(id);\n");
  CHECK_THROWS_AS(
      pg::validate_migration_resume(history, edited, dbdiff::sha256_hex(edited.sql), true),
      dbdiff::Error);
  CHECK_THROWS_AS(pg::validate_migration_resume(history, parsed, std::string(64U, 'f'), false),
                  dbdiff::Error);
}

TEST_CASE("PostgreSQL uncertain standalone units fail closed",
          "[unit][postgresql][history][MIG-002][PLN-004]") {
  const auto parsed = migration();
  auto history = incomplete_history(parsed);
  history.units.push_back(pg::MigrationUnitRecord{
      .ordinal = 1U,
      .exact_sha256 = parsed.units[1].exact_sha256,
      .explicit_transaction = false,
      .before_schema_sha256 = std::string(64U, 'b'),
      .after_schema_sha256 = {},
      .state = pg::MigrationUnitState::started,
  });

  CHECK_THROWS_AS(
      pg::validate_migration_resume(history, parsed, dbdiff::sha256_hex(parsed.sql), true),
      dbdiff::Error);

  history.units.pop_back();
  history.units[0].ordinal = 3U;
  CHECK_THROWS_AS(
      pg::validate_migration_resume(history, parsed, dbdiff::sha256_hex(parsed.sql), true),
      dbdiff::Error);
}

TEST_CASE("PostgreSQL migrations cannot access dbdiff metadata",
          "[unit][postgresql][history][MIG-002]") {
  CHECK_THROWS_AS(
      pg::parse_migration("BEGIN; CREATE TABLE \"_dbdiff\".user_visible(id bigint); COMMIT;"),
      dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_migration("DROP SCHEMA _dbdiff CASCADE;"), dbdiff::Error);
}

TEST_CASE("PostgreSQL history value objects retain exact recoverable revisions",
          "[unit][postgresql][history][MIG-004][APP-004]") {
  const auto parsed = migration();
  const pg::MigrationRevision revision{
      .ordinal = 2U,
      .exact_file_sha256 = dbdiff::sha256_hex(parsed.sql),
      .sql = parsed.sql,
  };
  const pg::MigrationHistory history{
      .initialized = true,
      .entries = {incomplete_history(parsed)},
  };

  CHECK(revision.sql == parsed.sql);
  CHECK(revision.exact_file_sha256 == dbdiff::sha256_hex(revision.sql));
  REQUIRE(history.entries.size() == 1U);
  CHECK(history.entries[0].units[0].exact_sha256 == parsed.units[0].exact_sha256);
}
