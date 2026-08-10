#include "dbdiff/docker.hpp"

#include "dbdiff/error.hpp"

#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace dbdiff::docker {
namespace {

constexpr std::size_t maximum_process_output = std::size_t{1024} * 1024U;
constexpr std::string_view owner_label = "io.dbdiff.scratch.owner";
constexpr std::string_view token_label = "io.dbdiff.scratch.token";
constexpr std::string_view backend_label = "io.dbdiff.scratch.backend";

[[noreturn]] void process_error(const std::string& message) {
  throw Error{ErrorCode::execution, message};
}

[[noreturn]] void docker_error(const std::string& message) {
  throw Error{ErrorCode::database, message};
}

void close_descriptor(int& descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
    descriptor = -1;
  }
}

void make_nonblocking(const int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
    process_error("could not configure process output capture");
  }
}

void append_bounded(std::string& destination, const char* data, const std::size_t size) {
  if (destination.size() >= maximum_process_output) {
    return;
  }
  const auto remaining = maximum_process_output - destination.size();
  destination.append(data, size < remaining ? size : remaining);
}

void drain_descriptor(const int descriptor, std::string& destination, bool& reached_end) {
  std::array<char, 4096> buffer{};
  while (!reached_end) {
    const auto bytes = ::read(descriptor, buffer.data(), buffer.size());
    if (bytes > 0) {
      append_bounded(destination, buffer.data(), static_cast<std::size_t>(bytes));
      continue;
    }
    if (bytes == 0) {
      reached_end = true;
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    process_error("could not read process output");
  }
}

void wait_after_kill(const pid_t process) noexcept {
  int status = 0;
  while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {
  }
}

class SystemProcessRunner final : public ProcessRunner {
public:
  ProcessResult run(const std::vector<std::string>& arguments,
                    const std::chrono::milliseconds timeout) override {
    if (arguments.empty()) {
      process_error("cannot execute an empty process argument list");
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
      process_error("process timeout must be positive");
    }
    for (const auto& argument : arguments) {
      if (argument.find('\0') != std::string::npos) {
        process_error("process arguments must not contain NUL bytes");
      }
    }

    std::vector<char*> native_arguments;
    native_arguments.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) {
      native_arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    native_arguments.push_back(nullptr);

    int standard_output_pipe[2]{-1, -1};
    int standard_error_pipe[2]{-1, -1};
    if (::pipe(standard_output_pipe) < 0) {
      process_error("could not create process output pipe");
    }
    if (::pipe(standard_error_pipe) < 0) {
      close_descriptor(standard_output_pipe[0]);
      close_descriptor(standard_output_pipe[1]);
      process_error("could not create process error pipe");
    }

    const pid_t process = ::fork();
    if (process < 0) {
      close_descriptor(standard_output_pipe[0]);
      close_descriptor(standard_output_pipe[1]);
      close_descriptor(standard_error_pipe[0]);
      close_descriptor(standard_error_pipe[1]);
      process_error("could not start process");
    }
    if (process == 0) {
      if (::dup2(standard_output_pipe[1], STDOUT_FILENO) < 0 ||
          ::dup2(standard_error_pipe[1], STDERR_FILENO) < 0) {
        ::_exit(127);
      }
      close_descriptor(standard_output_pipe[0]);
      close_descriptor(standard_output_pipe[1]);
      close_descriptor(standard_error_pipe[0]);
      close_descriptor(standard_error_pipe[1]);
      ::execvp(native_arguments[0], native_arguments.data());
      ::_exit(127);
    }

    close_descriptor(standard_output_pipe[1]);
    close_descriptor(standard_error_pipe[1]);

    bool process_reaped = false;
    try {
      make_nonblocking(standard_output_pipe[0]);
      make_nonblocking(standard_error_pipe[0]);

      ProcessResult result;
      bool output_ended = false;
      bool error_ended = false;
      int process_status = 0;
      const auto deadline = std::chrono::steady_clock::now() + timeout;

      while (true) {
        drain_descriptor(standard_output_pipe[0], result.standard_output, output_ended);
        drain_descriptor(standard_error_pipe[0], result.standard_error, error_ended);

        const auto waited = ::waitpid(process, &process_status, WNOHANG);
        if (waited == process) {
          process_reaped = true;
          break;
        }
        if (waited < 0 && errno != EINTR) {
          if (errno == ECHILD) {
            process_reaped = true;
          }
          process_error("could not wait for process");
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          static_cast<void>(::kill(process, SIGKILL));
          wait_after_kill(process);
          process_reaped = true;
          result.timed_out = true;
          break;
        }

        std::array<pollfd, 2> descriptors{{
            pollfd{standard_output_pipe[0], static_cast<short>(output_ended ? 0 : POLLIN), 0},
            pollfd{standard_error_pipe[0], static_cast<short>(error_ended ? 0 : POLLIN), 0},
        }};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto wait_time =
            remaining < std::chrono::milliseconds{50} ? remaining : std::chrono::milliseconds{50};
        const auto bounded_wait =
            wait_time.count() > static_cast<std::int64_t>(std::numeric_limits<int>::max())
                ? std::numeric_limits<int>::max()
                : static_cast<int>(wait_time.count());
        const int poll_result = ::poll(descriptors.data(), descriptors.size(), bounded_wait);
        if (poll_result < 0 && errno != EINTR) {
          process_error("could not poll process output");
        }
      }

      drain_descriptor(standard_output_pipe[0], result.standard_output, output_ended);
      drain_descriptor(standard_error_pipe[0], result.standard_error, error_ended);
      close_descriptor(standard_output_pipe[0]);
      close_descriptor(standard_error_pipe[0]);

      if (result.timed_out) {
        result.exit_code = 124;
      } else if (WIFEXITED(process_status)) {
        result.exit_code = WEXITSTATUS(process_status);
      } else if (WIFSIGNALED(process_status)) {
        result.exit_code = 128 + WTERMSIG(process_status);
      } else {
        result.exit_code = 1;
      }
      return result;
    } catch (...) {
      if (!process_reaped) {
        static_cast<void>(::kill(process, SIGKILL));
        wait_after_kill(process);
      }
      close_descriptor(standard_output_pipe[0]);
      close_descriptor(standard_error_pipe[0]);
      throw;
    }
  }
};

[[nodiscard]] std::string trim_ascii(const std::string& value) {
  constexpr std::string_view whitespace = " \t\r\n";
  const auto first = value.find_first_not_of(whitespace);
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(whitespace);
  return value.substr(first, last - first + 1U);
}

[[nodiscard]] bool is_lower_hex(const std::string_view value, const std::size_t expected_size) {
  if (value.size() != expected_size) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string random_hex() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    docker_error("could not generate Docker scratch credentials");
  }
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const unsigned char byte : bytes) {
    result.push_back(digits[static_cast<std::size_t>(byte >> 4U)]);
    result.push_back(digits[static_cast<std::size_t>(byte & 0x0fU)]);
  }
  return result;
}

void validate_options(const PostgresContainerOptions& options) {
  if (options.postgres_major < 15 || options.postgres_major > 18) {
    throw Error{ErrorCode::configuration, "Docker PostgreSQL major must be between 15 and 18"};
  }
  if (options.command_timeout <= std::chrono::milliseconds::zero()) {
    throw Error{ErrorCode::configuration, "Docker command timeout must be positive"};
  }
  if (options.readiness_timeout < std::chrono::milliseconds::zero() ||
      options.readiness_poll_interval < std::chrono::milliseconds::zero()) {
    throw Error{ErrorCode::configuration, "Docker readiness durations must not be negative"};
  }
  if (options.image) {
    if (options.image->empty() || options.image->find('\0') != std::string::npos) {
      throw Error{ErrorCode::configuration, "Docker image must be non-empty without NUL bytes"};
    }
  }
}

[[nodiscard]] std::string selected_image(const PostgresContainerOptions& options) {
  return options.image.value_or("postgres:" + std::to_string(options.postgres_major));
}

void require_success(const ProcessResult& result, const std::string_view operation) {
  if (result.timed_out) {
    docker_error(std::string{operation} + " timed out");
  }
  if (result.exit_code != 0) {
    docker_error(std::string{operation} + " failed");
  }
}

[[nodiscard]] unsigned short parse_host_port(const std::string& output) {
  const auto value = trim_ascii(output);
  constexpr std::string_view prefix = "127.0.0.1:";
  if (!value.starts_with(prefix) ||
      value.find_first_of("\r\n", prefix.size()) != std::string::npos) {
    docker_error("Docker returned an unexpected PostgreSQL port binding");
  }
  const auto port_text = std::string_view{value}.substr(prefix.size());
  unsigned int port = 0;
  const auto [end, error] =
      std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  if (error != std::errc{} || end != port_text.data() + port_text.size() || port == 0U ||
      port > std::numeric_limits<unsigned short>::max()) {
    docker_error("Docker returned an invalid PostgreSQL host port");
  }
  return static_cast<unsigned short>(port);
}

[[nodiscard]] int parse_server_version(const std::string& output, const int expected_major) {
  const auto value = trim_ascii(output);
  int version_number = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), version_number);
  if (error != std::errc{} || end != value.data() + value.size() ||
      version_number / 10000 != expected_major) {
    docker_error("Docker PostgreSQL server major does not match its configured major");
  }
  return version_number;
}

[[nodiscard]] std::string label_argument(const std::string_view key, const std::string_view value) {
  return std::string{key} + "=" + std::string{value};
}

} // namespace

struct PostgresContainer::Impl {
  PostgresContainerOptions options;
  std::shared_ptr<ProcessRunner> runner;
  std::string container_id;
  std::string ownership_token;
  std::string image;
  std::string database;
  std::string username;
  std::string password;
  unsigned short host_port{};
  int server_version_number{};
  bool open{true};
};

bool ProcessResult::succeeded() const noexcept { return exit_code == 0 && !timed_out; }

std::shared_ptr<ProcessRunner> make_system_process_runner() {
  return std::make_shared<SystemProcessRunner>();
}

PostgresContainer PostgresContainer::create(PostgresContainerOptions options,
                                            std::shared_ptr<ProcessRunner> runner) {
  validate_options(options);
  if (!runner) {
    throw Error{ErrorCode::configuration, "Docker process runner must not be null"};
  }

  const auto token = random_hex();
  const auto password = random_hex();
  const auto image_name = selected_image(options);
  const auto database_name = "dbdiff_" + token.substr(0, 16);
  constexpr std::string_view user_name = "dbdiff";
  const auto container_name = "dbdiff-scratch-" + token;

  const std::vector<std::string> run_arguments{
      "docker",
      "run",
      "--detach",
      "--name",
      container_name,
      "--label",
      label_argument(owner_label, "dbdiff"),
      "--label",
      label_argument(token_label, token),
      "--label",
      label_argument(backend_label, "postgresql"),
      "--env",
      std::string{"POSTGRES_USER="} + std::string{user_name},
      "--env",
      "POSTGRES_PASSWORD=" + password,
      "--env",
      "POSTGRES_DB=" + database_name,
      "--publish",
      "127.0.0.1::5432",
      image_name,
  };
  const auto run_result = runner->run(run_arguments, options.command_timeout);
  require_success(run_result, "Docker PostgreSQL container creation");
  auto container_id = trim_ascii(run_result.standard_output);
  if (!is_lower_hex(container_id, 64)) {
    docker_error("Docker returned an invalid container ID");
  }

  auto implementation = std::make_unique<Impl>(Impl{
      .options = std::move(options),
      .runner = std::move(runner),
      .container_id = std::move(container_id),
      .ownership_token = token,
      .image = image_name,
      .database = database_name,
      .username = std::string{user_name},
      .password = password,
  });
  PostgresContainer container{std::move(implementation)};

  try {
    const auto& state = container.require_impl();
    const auto port_result = state.runner->run({"docker", "port", state.container_id, "5432/tcp"},
                                               state.options.command_timeout);
    require_success(port_result, "Docker PostgreSQL port discovery");
    container.require_impl().host_port = parse_host_port(port_result.standard_output);

    const auto readiness_deadline =
        std::chrono::steady_clock::now() + state.options.readiness_timeout;
    while (true) {
      const auto version_result = state.runner->run(
          {"docker", "exec", "--env", "PGPASSWORD=" + state.password, state.container_id, "psql",
           "--host", "127.0.0.1", "--username", state.username, "--dbname", state.database,
           "--tuples-only", "--no-align", "--set", "ON_ERROR_STOP=1", "--command",
           "SHOW server_version_num;"},
          state.options.command_timeout);
      if (version_result.succeeded()) {
        container.require_impl().server_version_number =
            parse_server_version(version_result.standard_output, state.options.postgres_major);
        return container;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= readiness_deadline) {
        docker_error("Docker PostgreSQL container did not become ready");
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(readiness_deadline - now);
      const auto pause = state.options.readiness_poll_interval < remaining
                             ? state.options.readiness_poll_interval
                             : remaining;
      if (pause > std::chrono::milliseconds::zero()) {
        std::this_thread::sleep_for(pause);
      }
    }
  } catch (...) {
    container.close_best_effort();
    throw;
  }
}

PostgresContainer::PostgresContainer(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

PostgresContainer::PostgresContainer(PostgresContainer&& other) noexcept = default;

PostgresContainer& PostgresContainer::operator=(PostgresContainer&& other) noexcept {
  if (this != &other) {
    close_best_effort();
    implementation_ = std::move(other.implementation_);
  }
  return *this;
}

PostgresContainer::~PostgresContainer() { close_best_effort(); }

bool PostgresContainer::is_open() const noexcept {
  return implementation_ != nullptr && implementation_->open;
}

std::string_view PostgresContainer::container_id() const { return require_impl().container_id; }

std::string_view PostgresContainer::ownership_token() const {
  return require_impl().ownership_token;
}

std::string_view PostgresContainer::image() const { return require_impl().image; }

std::string_view PostgresContainer::database() const { return require_impl().database; }

std::string_view PostgresContainer::username() const { return require_impl().username; }

unsigned short PostgresContainer::host_port() const { return require_impl().host_port; }

int PostgresContainer::server_version_number() const {
  return require_impl().server_version_number;
}

std::string PostgresContainer::connection_dsn() const {
  const auto& state = require_impl();
  return "postgresql://" + state.username + ":" + state.password +
         "@127.0.0.1:" + std::to_string(state.host_port) + "/" + state.database;
}

std::string PostgresContainer::redacted_dsn() const {
  const auto& state = require_impl();
  return "postgresql://" + state.username + ":***@127.0.0.1:" + std::to_string(state.host_port) +
         "/" + state.database;
}

void PostgresContainer::close() {
  if (!implementation_ || !implementation_->open) {
    return;
  }
  auto& state = *implementation_;

  const auto inspect_label = [&state](const std::string_view key) {
    const auto format = "{{ index .Config.Labels \"" + std::string{key} + "\" }}";
    const auto result = state.runner->run(
        {"docker", "inspect", "--type", "container", "--format", format, state.container_id},
        state.options.command_timeout);
    require_success(result, "Docker scratch ownership inspection");
    return trim_ascii(result.standard_output);
  };

  if (inspect_label(owner_label) != "dbdiff" ||
      inspect_label(token_label) != state.ownership_token ||
      inspect_label(backend_label) != "postgresql") {
    docker_error("refusing to remove a Docker container with mismatched ownership labels");
  }

  const auto remove_result = state.runner->run(
      {"docker", "rm", "--force", "--volumes", state.container_id}, state.options.command_timeout);
  require_success(remove_result, "Docker scratch container removal");
  state.open = false;
}

PostgresContainer::Impl& PostgresContainer::require_impl() {
  if (!implementation_) {
    throw Error{ErrorCode::execution, "Docker PostgreSQL container has been moved"};
  }
  return *implementation_;
}

const PostgresContainer::Impl& PostgresContainer::require_impl() const {
  if (!implementation_) {
    throw Error{ErrorCode::execution, "Docker PostgreSQL container has been moved"};
  }
  return *implementation_;
}

void PostgresContainer::close_best_effort() noexcept {
  try {
    close();
  } catch (...) {
    const auto ignored = std::current_exception();
    static_cast<void>(ignored);
  }
}

} // namespace dbdiff::docker
