#include "dbdiff/docker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
namespace docker = dbdiff::docker;

namespace {

int selected_major() {
  const char* value = std::getenv("DBDIFF_TEST_POSTGRES_MAJOR"); // NOLINT(concurrency-mt-unsafe)
  if (value == nullptr || *value == '\0') {
    return 18;
  }
  const std::string_view text{value};
  int major = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), major);
  if (error != std::errc{} || end != text.data() + text.size() || major < 15 || major > 18) {
    FAIL("DBDIFF_TEST_POSTGRES_MAJOR must be 15, 16, 17, or 18");
  }
  return major;
}

} // namespace

TEST_CASE("System process runner preserves argv boundaries and enforces timeout",
          "[integration][process][OPS-001]") {
  auto runner = docker::make_system_process_runner();
  const auto literal =
      runner->run({"/usr/bin/printf", "%s", "$(printf must-not-expand); echo must-not-run"}, 10s);
  REQUIRE(literal.succeeded());
  CHECK(literal.standard_output == "$(printf must-not-expand); echo must-not-run");

  const auto timed_out = runner->run({"/bin/sleep", "1"}, 10ms);
  CHECK(timed_out.timed_out);
  CHECK(timed_out.exit_code == 124);
}

TEST_CASE("Docker PostgreSQL scratch is reachable and explicitly removable",
          "[integration][docker][SCR-002][SCR-004][SCR-005]") {
  auto runner = docker::make_system_process_runner();
  const auto docker_status = runner->run({"docker", "info", "--format", "{{.ServerVersion}}"}, 10s);
  if (!docker_status.succeeded()) {
    SKIP("Docker CLI or daemon is unavailable");
  }

  docker::PostgresContainerOptions options;
  options.postgres_major = selected_major();
  options.command_timeout = 5min;
  options.readiness_timeout = 90s;
  if (const char* image =
          std::getenv("DBDIFF_TEST_POSTGRES_IMAGE"); // NOLINT(concurrency-mt-unsafe)
      image != nullptr && *image != '\0') {
    options.image = image;
  }

  auto container = docker::PostgresContainer::create(options, runner);
  INFO("scratch " << container.redacted_dsn());
  CHECK(container.is_open());
  CHECK(container.host_port() != 0);
  CHECK(container.server_version_number() / 10000 == options.postgres_major);
  CHECK(container.redacted_dsn().find(":***@") != std::string::npos);

  const auto query =
      runner->run({"docker", "exec", std::string{container.container_id()}, "psql", "--username",
                   std::string{container.username()}, "--dbname", std::string{container.database()},
                   "--tuples-only", "--no-align", "--command", "SELECT current_database();"},
                  30s);
  REQUIRE(query.succeeded());
  CHECK(query.standard_output.find(container.database()) != std::string::npos);

  container.close();
  CHECK_FALSE(container.is_open());
  const auto inspect =
      runner->run({"docker", "inspect", std::string{container.container_id()}}, 10s);
  CHECK_FALSE(inspect.succeeded());
}
