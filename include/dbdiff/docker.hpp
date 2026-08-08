#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff::docker {

struct ProcessResult {
  int exit_code{};
  bool timed_out{false};
  std::string standard_output;
  std::string standard_error;

  [[nodiscard]] bool succeeded() const noexcept;
};

class ProcessRunner {
public:
  virtual ~ProcessRunner() = default;

  [[nodiscard]] virtual ProcessResult run(const std::vector<std::string>& arguments,
                                          std::chrono::milliseconds timeout) = 0;
};

[[nodiscard]] std::shared_ptr<ProcessRunner> make_system_process_runner();

struct PostgresContainerOptions {
  int postgres_major{18};
  std::optional<std::string> image;
  std::chrono::milliseconds command_timeout{300000};
  std::chrono::milliseconds readiness_timeout{90000};
  std::chrono::milliseconds readiness_poll_interval{250};
};

class PostgresContainer {
public:
  [[nodiscard]] static PostgresContainer
  create(PostgresContainerOptions options,
         std::shared_ptr<ProcessRunner> runner = make_system_process_runner());

  PostgresContainer(PostgresContainer&& other) noexcept;
  PostgresContainer& operator=(PostgresContainer&& other) noexcept;
  PostgresContainer(const PostgresContainer&) = delete;
  PostgresContainer& operator=(const PostgresContainer&) = delete;
  ~PostgresContainer();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::string_view container_id() const;
  [[nodiscard]] std::string_view ownership_token() const;
  [[nodiscard]] std::string_view image() const;
  [[nodiscard]] std::string_view database() const;
  [[nodiscard]] std::string_view username() const;
  [[nodiscard]] unsigned short host_port() const;
  [[nodiscard]] int server_version_number() const;
  [[nodiscard]] std::string connection_dsn() const;
  [[nodiscard]] std::string redacted_dsn() const;

  // Verifies every ownership label, then removes this exact container and its anonymous volumes.
  // Throws on a missing/mismatched label or a Docker CLI failure.
  void close();

private:
  struct Impl;

  explicit PostgresContainer(std::unique_ptr<Impl> implementation);
  [[nodiscard]] Impl& require_impl();
  [[nodiscard]] const Impl& require_impl() const;
  void close_best_effort() noexcept;

  std::unique_ptr<Impl> implementation_;
};

} // namespace dbdiff::docker
