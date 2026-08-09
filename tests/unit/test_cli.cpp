#include "dbdiff/cli.hpp"
#include "dbdiff/error.hpp"

#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Invocation {
  int status{0};
  std::string output;
  std::string error;
};

Invocation invoke(const std::vector<std::string>& arguments,
                  const std::filesystem::path& working_directory, const dbdiff::Runtime& runtime) {
  auto mutable_arguments = arguments;
  std::vector<char*> argv;
  argv.reserve(mutable_arguments.size());
  for (auto& argument : mutable_arguments) {
    argv.push_back(argument.data());
  }
  std::ostringstream output;
  std::ostringstream error;
  const auto status = dbdiff::run_cli(static_cast<int>(argv.size()), argv.data(), output, error,
                                      working_directory, runtime);
  return Invocation{status, output.str(), error.str()};
}

dbdiff::Runtime runtime() {
  return dbdiff::Runtime{
      .environment = [](std::string_view) { return std::optional<std::string>{}; },
      .stdin_reader = [] { return std::string{}; },
      .now = [] { return std::chrono::system_clock::time_point{}; },
  };
}

} // namespace

TEST_CASE("CLI help and version return success", "[unit][CLI-001]") {
  dbdiff::test::TempDirectory directory;
  const auto help = invoke({"dbdiff", "--help"}, directory.path(), runtime());
  CHECK(help.status == 0);
  CHECK(help.output.find("create") != std::string::npos);
  CHECK(help.output.find("apply") != std::string::npos);

  const auto version = invoke({"dbdiff", "--version"}, directory.path(), runtime());
  CHECK(version.status == 0);
  CHECK(version.output.find("0.1.0") != std::string::npos);
}

TEST_CASE("CLI discovers configuration and validates hazards", "[unit][CLI-001][CFG-002]") {
  dbdiff::test::TempDirectory directory;
  std::filesystem::create_directories(directory.path() / ".git");
  std::filesystem::create_directories(directory.path() / "nested");
  directory.write("schema.sql", "CREATE TABLE items(id INTEGER PRIMARY KEY);\n");
  directory.write("dbdiff.yaml", R"yaml(format: 1
backend: sqlite
sources: [schema.sql]
migrations: migrations
)yaml");

  const auto invalid =
      invoke({"dbdiff", "create", "--name", "initial", "--allow-hazard", "not-a-hazard"},
             directory.path() / "nested", runtime());
  CHECK(invalid.status == 1);
  CHECK(invalid.error.find("unknown hazard") != std::string::npos);

  const auto created =
      invoke({"dbdiff", "create", "--name", "initial", "--allow-hazard", "WRITE_LOCK"},
             directory.path() / "nested", runtime());
  CHECK(created.status == 0);
  CHECK(created.output.find("Saved migration") != std::string::npos);
}

TEST_CASE("CLI reports missing configuration as an operational failure", "[unit][CLI-002]") {
  dbdiff::test::TempDirectory directory;
  const auto result =
      invoke({"dbdiff", "create", "--name", "initial"}, directory.path(), runtime());
  CHECK(result.status == 1);
  CHECK(result.error.find("no dbdiff.yaml") != std::string::npos);
}
