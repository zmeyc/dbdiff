cmake_minimum_required(VERSION 3.24)

project(dbdiff VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(DBDIFF_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DBDIFF_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(DBDIFF_BUILD_INTEGRATION_TESTS "Build tests that require external database engines" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

if(DBDIFF_ENABLE_CLANG_TIDY)
  find_program(DBDIFF_CLANG_TIDY NAMES clang-tidy-18 clang-tidy REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY "${DBDIFF_CLANG_TIDY};--config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET libpqxx>=7.8)
find_package(SQLite3 3.45 REQUIRED)
find_package(CLI11 2.4 CONFIG REQUIRED)
find_package(yaml-cpp 0.8 CONFIG REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

if(TARGET yaml-cpp::yaml-cpp)
  set(DBDIFF_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
  set(DBDIFF_YAML_TARGET yaml-cpp)
else()
  message(FATAL_ERROR "yaml-cpp did not provide a supported CMake target")
endif()

add_library(
  dbdiff_core STATIC
  src/core/config.cpp
  src/core/error.cpp
  src/core/hazard.cpp
  src/core/hash.cpp
  src/core/migration.cpp
  src/core/operation.cpp
  src/core/script.cpp
  src/core/source.cpp
  src/core/version.cpp)
add_library(dbdiff::core ALIAS dbdiff_core)
target_include_directories(dbdiff_core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(dbdiff_core PUBLIC OpenSSL::Crypto ${DBDIFF_YAML_TARGET})
dbdiff_set_warnings(dbdiff_core)
dbdiff_enable_sanitizers(dbdiff_core)

add_library(dbdiff_sqlite STATIC src/sqlite/backend.cpp)
add_library(dbdiff::sqlite ALIAS dbdiff_sqlite)
target_include_directories(dbdiff_sqlite PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(dbdiff_sqlite PUBLIC dbdiff_core SQLite::SQLite3)
dbdiff_set_warnings(dbdiff_sqlite)
dbdiff_enable_sanitizers(dbdiff_sqlite)

add_library(dbdiff_postgresql STATIC src/postgresql/backend.cpp)
add_library(dbdiff::postgresql ALIAS dbdiff_postgresql)
target_include_directories(dbdiff_postgresql PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(dbdiff_postgresql PUBLIC dbdiff_core PkgConfig::PQXX)
dbdiff_set_warnings(dbdiff_postgresql)
dbdiff_enable_sanitizers(dbdiff_postgresql)

add_executable(dbdiff src/main.cpp)
target_link_libraries(dbdiff PRIVATE dbdiff_sqlite dbdiff_postgresql CLI11::CLI11)
dbdiff_set_warnings(dbdiff)
dbdiff_enable_sanitizers(dbdiff)

install(TARGETS dbdiff RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  find_package(Catch2 3.4 CONFIG REQUIRED)
  add_subdirectory(tests)
endif()

find_program(DBDIFF_CLANG_FORMAT NAMES clang-format-18 clang-format)
if(DBDIFF_CLANG_FORMAT)
  file(GLOB_RECURSE DBDIFF_FORMAT_FILES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
       "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  add_custom_target(
    format
    COMMAND "${DBDIFF_CLANG_FORMAT}" -i ${DBDIFF_FORMAT_FILES}
    COMMAND_EXPAND_LISTS
    VERBATIM)
  add_custom_target(
    format-check
    COMMAND "${DBDIFF_CLANG_FORMAT}" --dry-run --Werror ${DBDIFF_FORMAT_FILES}
    COMMAND_EXPAND_LISTS
    VERBATIM)
endif()
