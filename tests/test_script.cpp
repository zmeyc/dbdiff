#include "dbdiff/error.hpp"
#include "dbdiff/script.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

dbdiff::StatementSpan span(const std::string& sql, const std::string& statement,
                           const dbdiff::StatementKind kind) {
  const auto begin = sql.find(statement);
  REQUIRE(begin != std::string::npos);
  return {begin, begin + statement.size(), kind};
}

} // namespace

TEST_CASE("execution units preserve visible transaction boundaries") {
  const std::string sql = "BEGIN;\nCREATE TABLE a(id INT);\nCOMMIT;\nCREATE INDEX i ON a(id);\n";
  auto parsed = dbdiff::build_execution_units(
      sql, {span(sql, "BEGIN;", dbdiff::StatementKind::begin),
            span(sql, "CREATE TABLE a(id INT);", dbdiff::StatementKind::ddl),
            span(sql, "COMMIT;", dbdiff::StatementKind::commit),
            span(sql, "CREATE INDEX i ON a(id);", dbdiff::StatementKind::ddl)});

  REQUIRE(parsed.units.size() == 2);
  CHECK(parsed.units[0].explicit_transaction);
  CHECK(parsed.units[0].statements.size() == 3);
  CHECK_FALSE(parsed.units[1].explicit_transaction);
  CHECK(parsed.units[1].statements.size() == 1);
  CHECK(parsed.sql.substr(parsed.units[0].begin, parsed.units[0].end - parsed.units[0].begin) ==
        "BEGIN;\nCREATE TABLE a(id INT);\nCOMMIT;");
}

TEST_CASE("execution units reject ambiguous transaction control") {
  const std::string nested = "BEGIN; BEGIN; COMMIT;";
  CHECK_THROWS_AS(dbdiff::build_execution_units(
                      nested, {span(nested, "BEGIN;", dbdiff::StatementKind::begin),
                               {7, 13, dbdiff::StatementKind::begin},
                               span(nested, "COMMIT;", dbdiff::StatementKind::commit)}),
                  dbdiff::Error);

  const std::string unmatched = "COMMIT;";
  CHECK_THROWS_AS(dbdiff::build_execution_units(
                      unmatched, {span(unmatched, "COMMIT;", dbdiff::StatementKind::commit)}),
                  dbdiff::Error);

  const std::string open = "BEGIN; CREATE TABLE t(id INT);";
  CHECK_THROWS_AS(dbdiff::build_execution_units(
                      open, {span(open, "BEGIN;", dbdiff::StatementKind::begin),
                             span(open, "CREATE TABLE t(id INT);", dbdiff::StatementKind::ddl)}),
                  dbdiff::Error);

  const std::string rollback = "ROLLBACK;";
  CHECK_THROWS_AS(dbdiff::build_execution_units(
                      rollback, {span(rollback, "ROLLBACK;", dbdiff::StatementKind::rollback)}),
                  dbdiff::Error);
}

TEST_CASE("standalone DML and overlapping statement spans are rejected") {
  const std::string sql = "INSERT INTO t VALUES (1);";
  CHECK_THROWS_AS(dbdiff::build_execution_units(sql, {span(sql, sql, dbdiff::StatementKind::dml)}),
                  dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::build_execution_units("abcdef", {{0, 4, dbdiff::StatementKind::ddl},
                                                           {3, 6, dbdiff::StatementKind::ddl}}),
                  dbdiff::Error);
}

TEST_CASE("a completed execution prefix cannot be edited") {
  const std::string sql = "CREATE TABLE a(id INT); CREATE TABLE b(id INT);";
  const auto parsed = dbdiff::build_execution_units(
      sql, {span(sql, "CREATE TABLE a(id INT);", dbdiff::StatementKind::ddl),
            span(sql, "CREATE TABLE b(id INT);", dbdiff::StatementKind::ddl)});
  REQUIRE(parsed.units.size() == 2);
  CHECK_NOTHROW(dbdiff::validate_completed_prefix({parsed.units[0].exact_sha256}, parsed));
  CHECK_THROWS_AS(dbdiff::validate_completed_prefix({"changed"}, parsed), dbdiff::Error);
  CHECK_THROWS_AS(
      dbdiff::validate_completed_prefix(
          {parsed.units[0].exact_sha256, parsed.units[1].exact_sha256, "extra"}, parsed),
      dbdiff::Error);
}
