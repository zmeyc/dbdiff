include(Catch)

add_executable(dbdiff_unit_tests test_smoke.cpp)
target_link_libraries(dbdiff_unit_tests PRIVATE dbdiff_core Catch2::Catch2WithMain)
dbdiff_set_warnings(dbdiff_unit_tests)
dbdiff_enable_sanitizers(dbdiff_unit_tests)

catch_discover_tests(dbdiff_unit_tests)

