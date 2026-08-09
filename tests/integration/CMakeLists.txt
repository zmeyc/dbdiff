add_executable(dbdiff_docker_integration_tests docker_scratch.cpp)
target_link_libraries(dbdiff_docker_integration_tests PRIVATE dbdiff_core Catch2::Catch2WithMain)
dbdiff_set_warnings(dbdiff_docker_integration_tests)
dbdiff_enable_sanitizers(dbdiff_docker_integration_tests)

catch_discover_tests(
  dbdiff_docker_integration_tests
  TEST_PREFIX "integration.docker."
  PROPERTIES LABELS integration.docker)

find_program(DBDIFF_BASH_EXECUTABLE NAMES bash)
if(DBDIFF_BASH_EXECUTABLE)
  add_test(
    NAME dbdiff_cli_lifecycle_integration
    COMMAND "${DBDIFF_BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cli_lifecycle.sh"
            "$<TARGET_FILE:dbdiff>")
  set_tests_properties(
    dbdiff_cli_lifecycle_integration
    PROPERTIES LABELS "integration;integration.sqlite;cli"
               SKIP_RETURN_CODE 77
               TIMEOUT 180)
else()
  message(STATUS "bash not found; SQLite CLI lifecycle integration test is unavailable")
endif()
