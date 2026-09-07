#include "dbdiff/error.hpp"
#include "dbdiff/operation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("operation ordering is topological and deterministic", "[unit][PLN-001]") {
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

TEST_CASE("operation ordering rejects invalid dependency graphs", "[unit][PLN-001]") {
  const std::vector<dbdiff::Operation> empty_id{{.id = ""}};
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(empty_id), dbdiff::Error);

  const std::vector<dbdiff::Operation> duplicate_ids{{.id = "a"}, {.id = "a"}};
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(duplicate_ids), dbdiff::Error);

  const std::vector<dbdiff::Operation> missing_dependency{{.id = "a", .dependencies = {"missing"}}};
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(missing_dependency), dbdiff::Error);

  const std::vector<dbdiff::Operation> cycle{{.id = "a", .dependencies = {"b"}},
                                             {.id = "b", .dependencies = {"a"}}};
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(cycle), dbdiff::Error);

  const std::vector<dbdiff::Operation> duplicate_dependencies{
      {.id = "a"}, {.id = "b", .dependencies = {"a", "a"}}};
  CHECK_THROWS_AS(dbdiff::deterministic_operation_order(duplicate_dependencies), dbdiff::Error);
}
