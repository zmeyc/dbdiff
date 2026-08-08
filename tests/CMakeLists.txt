include(Catch)

add_executable(
  dbdiff_unit_tests
  test_config.cpp
  unit/test_docker.cpp
  test_hash.cpp
  test_migration.cpp
  test_operation.cpp
  test_script.cpp
  test_smoke.cpp
  test_source.cpp
  unit/test_postgresql.cpp
  unit/test_postgresql_planner.cpp
  unit/test_sqlite.cpp
  unit/test_sqlite_planner.cpp)
target_link_libraries(dbdiff_unit_tests PRIVATE dbdiff_postgresql dbdiff_sqlite
                                                Catch2::Catch2WithMain)
dbdiff_set_warnings(dbdiff_unit_tests)
dbdiff_enable_sanitizers(dbdiff_unit_tests)

catch_discover_tests(dbdiff_unit_tests)

if(DBDIFF_BUILD_INTEGRATION_TESTS)
  add_subdirectory(integration)
endif()
