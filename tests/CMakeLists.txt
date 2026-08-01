include(Catch)

add_executable(
  dbdiff_unit_tests
  test_config.cpp
  test_hash.cpp
  test_migration.cpp
  test_operation.cpp
  test_script.cpp
  test_smoke.cpp
  test_source.cpp)
target_link_libraries(dbdiff_unit_tests PRIVATE dbdiff_core Catch2::Catch2WithMain)
dbdiff_set_warnings(dbdiff_unit_tests)
dbdiff_enable_sanitizers(dbdiff_unit_tests)

catch_discover_tests(dbdiff_unit_tests)
