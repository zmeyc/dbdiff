#include "dbdiff/error.hpp"
#include "dbdiff/operation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("operation ordering is topological and deterministic") {
  const std::vector<dbdiff::Operation> operations{
      {.id = "table", .dependencies = {"schema"}},
      {.id = "z_independent"},
      {.id = "schema"},
      {.id = "a_independent"},
      {.id = "index", .dependencies = {"table"}},
  };
  const auto order = dbdiff::deterministic_operation_order(operations);
  CHECK(order == std::vector<std::size_t>{3, 2, 0, 4, 1});
}

TEST_CASE("operation ordering rejects invalid dependency graphs") {
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order({{{.id = ""}}}), dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order({{{.id = "a"}, {.id = "a"}}}),
                  dbdiff::Error);
  CHECK_THROWS_AS(
      dbdiff::deterministic_operation_order({{{.id = "a", .dependencies = {"missing"}}}}),
      dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(
                      {{{.id = "a", .dependencies = {"b"}}, {.id = "b", .dependencies = {"a"}}}}),
                  dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(
                      {{{.id = "a"}, {.id = "b", .dependencies = {"a", "a"}}}}),
                  dbdiff::Error);
}
