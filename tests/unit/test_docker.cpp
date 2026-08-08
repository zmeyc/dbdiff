#include "dbdiff/docker.hpp"

#include "dbdiff/error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace docker = dbdiff::docker;

namespace {

constexpr std::string_view container_id =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

[[nodiscard]] docker::ProcessResult success(std::string output = {}) {
  return docker::ProcessResult{0, false, std::move(output), {}};
}

[[nodiscard]] std::optional<std::string>
argument_with_prefix(const std::vector<std::string>& arguments, const std::string_view prefix) {
  const auto found = std::ranges::find_if(
      arguments, [prefix](const std::string& argument) { return argument.starts_with(prefix); });
  if (found == arguments.end()) {
    return std::nullopt;
  }
  return found->substr(prefix.size());
}

class FakeProcessRunner final : public docker::ProcessRunner {
public:
  docker::ProcessResult run(const std::vector<std::string>& arguments,
                            std::chrono::milliseconds) override {
    calls.push_back(arguments);
    if (arguments.size() < 2 || arguments[0] != "docker") {
      return docker::ProcessResult{99, false, {}, "unexpected executable"};
    }

    if (arguments[1] == "run") {
      run_arguments = arguments;
      ownership_token = argument_with_prefix(arguments, "io.dbdiff.scratch.token=").value_or("");
      password = argument_with_prefix(arguments, "POSTGRES_PASSWORD=").value_or("");
      return success(std::string{container_id} + "\n");
    }
    if (arguments[1] == "port") {
      return success("127.0.0.1:49152\n");
    }
    if (arguments[1] == "exec") {
      if (arguments.at(3) == "pg_isready") {
        ++readiness_calls;
        if (readiness_failures > 0) {
          --readiness_failures;
          return docker::ProcessResult{1, false, {}, "not ready"};
        }
        return success();
      }
      if (arguments.at(3) == "psql") {
        ++version_calls;
        return success(std::to_string(server_version_number) + "\n");
      }
      return docker::ProcessResult{97, false, {}, "unexpected exec command"};
    }
    if (arguments[1] == "inspect") {
      ++inspection_calls;
      const auto& format = arguments.at(5);
      if (format.find("io.dbdiff.scratch.owner") != std::string::npos) {
        return success(mismatch_labels ? "someone-else\n" : "dbdiff\n");
      }
      if (format.find("io.dbdiff.scratch.token") != std::string::npos) {
        return success(mismatch_labels ? "wrong-token\n" : ownership_token + "\n");
      }
      if (format.find("io.dbdiff.scratch.backend") != std::string::npos) {
        return success(mismatch_labels ? "sqlite\n" : "postgresql\n");
      }
      return docker::ProcessResult{2, false, {}, "unknown label"};
    }
    if (arguments[1] == "rm") {
      ++remove_calls;
      remove_arguments = arguments;
      return remove_fails ? docker::ProcessResult{1, false, {}, "remove failed"} : success();
    }
    return docker::ProcessResult{98, false, {}, "unexpected Docker command"};
  }

  std::vector<std::vector<std::string>> calls;
  std::vector<std::string> run_arguments;
  std::vector<std::string> remove_arguments;
  std::string ownership_token;
  std::string password;
  int readiness_failures{};
  int readiness_calls{};
  int version_calls{};
  int server_version_number{180000};
  int inspection_calls{};
  int remove_calls{};
  bool mismatch_labels{false};
  bool remove_fails{false};
};

docker::PostgresContainerOptions test_options(const int major) {
  docker::PostgresContainerOptions options;
  options.postgres_major = major;
  options.command_timeout = 1s;
  options.readiness_timeout = 100ms;
  options.readiness_poll_interval = 0ms;
  return options;
}

} // namespace

TEST_CASE("Docker PostgreSQL commands preserve argv boundaries and secrets",
          "[unit][docker][SCR-002][SCR-005]") {
  for (const int major : {15, 16, 17, 18}) {
    DYNAMIC_SECTION("PostgreSQL " << major) {
      auto runner = std::make_shared<FakeProcessRunner>();
      runner->server_version_number = major * 10000;
      auto options = test_options(major);
      options.image = "registry.example/postgres:" + std::to_string(major) + ";not-a-shell";

      auto container = docker::PostgresContainer::create(options, runner);

      REQUIRE_FALSE(runner->run_arguments.empty());
      CHECK(runner->run_arguments[0] == "docker");
      CHECK(runner->run_arguments[1] == "run");
      CHECK(runner->run_arguments.back() == options.image.value());
      CHECK(std::ranges::find(runner->run_arguments, "/bin/sh") == runner->run_arguments.end());
      CHECK(std::ranges::find(runner->run_arguments, "-c") == runner->run_arguments.end());
      CHECK(argument_with_prefix(runner->run_arguments, "io.dbdiff.scratch.owner=") == "dbdiff");
      CHECK(argument_with_prefix(runner->run_arguments, "io.dbdiff.scratch.backend=") ==
            "postgresql");
      REQUIRE(runner->ownership_token.size() == 32);
      REQUIRE(runner->password.size() == 32);
      CHECK(container.container_id() == container_id);
      CHECK(container.ownership_token() == runner->ownership_token);
      CHECK(container.host_port() == 49152);
      CHECK(container.server_version_number() / 10000 == major);
      CHECK(container.image() == options.image.value());
      CHECK(container.connection_dsn().find(runner->password) != std::string::npos);
      CHECK(container.redacted_dsn().find(runner->password) == std::string::npos);
      CHECK(container.redacted_dsn().find(":***@127.0.0.1:49152/") != std::string::npos);

      container.close();
      CHECK_FALSE(container.is_open());
      CHECK(runner->inspection_calls == 3);
      CHECK(runner->remove_calls == 1);
      CHECK(runner->remove_arguments == std::vector<std::string>{"docker", "rm", "--force",
                                                                 "--volumes",
                                                                 std::string{container_id}});
    }
  }
}

TEST_CASE("Docker PostgreSQL readiness retries without using a shell", "[unit][docker][SCR-002]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  runner->readiness_failures = 2;

  auto container = docker::PostgresContainer::create(test_options(18), runner);
  CHECK(runner->readiness_calls == 3);
  for (const auto& call : runner->calls) {
    CHECK(std::ranges::find(call, "/bin/sh") == call.end());
    CHECK(std::ranges::find(call, "-c") == call.end());
  }
  container.close();
}

TEST_CASE("Docker PostgreSQL creation failure cleans only verified ownership",
          "[unit][docker][SCR-004]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  runner->readiness_failures = 1;
  auto options = test_options(18);
  options.readiness_timeout = 0ms;

  CHECK_THROWS_AS(docker::PostgresContainer::create(options, runner), dbdiff::Error);
  CHECK(runner->inspection_calls == 3);
  CHECK(runner->remove_calls == 1);
  CHECK(runner->remove_arguments.back() == container_id);
}

TEST_CASE("Docker PostgreSQL close refuses a mismatched ownership label",
          "[unit][docker][SCR-004]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  {
    auto container = docker::PostgresContainer::create(test_options(18), runner);
    runner->mismatch_labels = true;
    CHECK_THROWS_AS(container.close(), dbdiff::Error);
    CHECK(container.is_open());
    CHECK(runner->remove_calls == 0);
  }
  CHECK(runner->remove_calls == 0);
}

TEST_CASE("Docker PostgreSQL destructor performs verified best-effort cleanup",
          "[unit][docker][SCR-004]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  {
    auto container = docker::PostgresContainer::create(test_options(18), runner);
    CHECK(container.is_open());
  }
  CHECK(runner->inspection_calls == 3);
  CHECK(runner->remove_calls == 1);
  CHECK(runner->remove_arguments.back() == container_id);
}

TEST_CASE(
    "Docker PostgreSQL explicit close reports removal failure while destruction is best effort",
    "[unit][docker][SCR-004]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  {
    auto container = docker::PostgresContainer::create(test_options(18), runner);
    runner->remove_fails = true;
    CHECK_THROWS_AS(container.close(), dbdiff::Error);
    CHECK(container.is_open());
  }
  CHECK(runner->remove_calls == 2);
}

TEST_CASE("Docker PostgreSQL options reject unsupported or unsafe input before execution",
          "[unit][docker][SCR-002]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  CHECK_THROWS_AS(docker::PostgresContainer::create(test_options(14), runner), dbdiff::Error);
  CHECK_THROWS_AS(docker::PostgresContainer::create(test_options(19), runner), dbdiff::Error);

  auto empty_image = test_options(18);
  empty_image.image = "";
  CHECK_THROWS_AS(docker::PostgresContainer::create(empty_image, runner), dbdiff::Error);

  auto invalid_timeout = test_options(18);
  invalid_timeout.command_timeout = 0ms;
  CHECK_THROWS_AS(docker::PostgresContainer::create(invalid_timeout, runner), dbdiff::Error);
  CHECK(runner->calls.empty());
}

TEST_CASE("Docker PostgreSQL rejects an image serving the wrong major and cleans it",
          "[unit][docker][SCR-002][SCR-004]") {
  auto runner = std::make_shared<FakeProcessRunner>();
  runner->server_version_number = 170009;

  CHECK_THROWS_AS(docker::PostgresContainer::create(test_options(18), runner), dbdiff::Error);
  CHECK(runner->version_calls == 1);
  CHECK(runner->inspection_calls == 3);
  CHECK(runner->remove_calls == 1);
}
