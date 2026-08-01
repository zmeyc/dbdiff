#include "dbdiff/backend.hpp"
#include "dbdiff/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("version and backend names are available") {
  CHECK(dbdiff::version() == "0.1.0");
  CHECK(dbdiff::backend_name(dbdiff::BackendKind::postgresql) == "postgresql");
  CHECK(dbdiff::backend_name(dbdiff::BackendKind::sqlite) == "sqlite");
}
