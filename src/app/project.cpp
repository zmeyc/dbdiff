#include "project.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace dbdiff::app_detail {

[[noreturn]] void lifecycle_error(const ErrorCode code, const std::string& message) {
  throw Error{code, message};
}

ProjectLifecycleLock::ProjectLifecycleLock(const std::filesystem::path& config_file,
                                           const std::chrono::milliseconds timeout) {
  descriptor_ = ::open(config_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor_ == -1) {
    lifecycle_error(ErrorCode::configuration,
                    "cannot open configuration file for lifecycle locking");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  constexpr auto poll_interval = std::chrono::milliseconds{10};
  while (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
    const auto lock_error = errno;
    if (lock_error != EWOULDBLOCK && lock_error != EAGAIN && lock_error != EINTR) {
      close_descriptor();
      lifecycle_error(ErrorCode::database, "project lifecycle lock acquisition failed");
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      close_descriptor();
      lifecycle_error(ErrorCode::database, "project lifecycle lock acquisition timed out");
    }
    const auto poll =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(poll_interval);
    std::this_thread::sleep_for(std::min(deadline - now, poll));
  }
}

ProjectLifecycleLock::~ProjectLifecycleLock() {
  if (descriptor_ != -1) {
    static_cast<void>(::flock(descriptor_, LOCK_UN));
    close_descriptor();
  }
}

void ProjectLifecycleLock::close_descriptor() noexcept {
  if (descriptor_ != -1) {
    static_cast<void>(::close(descriptor_));
    descriptor_ = -1;
  }
}

sqlite::ConnectionSettings sqlite_settings(const Config& config) {
  return sqlite::ConnectionSettings{config.lock_timeout, config.statement_timeout};
}

postgresql::ConnectionSettings postgresql_settings(const Config& config) {
  return postgresql::ConnectionSettings{config.lock_timeout, config.statement_timeout};
}

ProjectInputs load_inputs(Config config, const Runtime& runtime) {
  SourceResolver resolver{config.file.parent_path(), config.migrations, runtime.stdin_reader};
  auto sources = resolver.resolve(config.sources);
  auto migrations = load_migrations(config.migrations, config.backend);
  return ProjectInputs{std::move(config), std::move(sources), std::move(migrations)};
}

ProjectInputs load_history_inputs(Config config) {
  auto migrations = load_migrations(config.migrations, config.backend);
  return ProjectInputs{std::move(config), {}, std::move(migrations)};
}

std::string require_target_locator(const Config& config, const Runtime& runtime) {
  if (config.database.empty()) {
    lifecycle_error(ErrorCode::configuration,
                    "this command requires database or database_env in the configuration");
  }
  const auto locator = resolve_locator(config.database, runtime.environment);
  if (!locator || locator->empty()) {
    lifecycle_error(ErrorCode::configuration, "configured database locator is unavailable");
  }
  return *locator;
}

std::filesystem::path sqlite_target_path(const Config& config, const Runtime& runtime) {
  const auto locator = require_target_locator(config, runtime);
  constexpr std::string_view prefix{"sqlite:"};
  if (!std::string_view{locator}.starts_with(prefix)) {
    lifecycle_error(ErrorCode::configuration, "SQLite database locators must start with 'sqlite:'");
  }
  const auto path_text = std::string_view{locator}.substr(prefix.size());
  if (path_text.empty() || path_text == ":memory:" || path_text.starts_with("file:") ||
      path_text.find_first_of("?#") != std::string_view::npos) {
    lifecycle_error(ErrorCode::configuration,
                    "SQLite target must be a plain persistent filesystem path");
  }
  std::filesystem::path path{path_text};
  if (!path.is_absolute()) {
    path = config.file.parent_path() / path;
  }
  return path.lexically_normal();
}

bool sqlite_target_exists(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      status.type() == std::filesystem::file_type::not_found) {
    return false;
  }
  if (error) {
    lifecycle_error(ErrorCode::database, "cannot inspect SQLite target path: " + error.message());
  }
  if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
    lifecycle_error(ErrorCode::database,
                    "SQLite target must be a regular file and not a symbolic link");
  }
  return true;
}

std::string concatenate_sources(const SourceSet& sources) {
  std::string sql;
  for (const auto& source : sources.files) {
    sql.push_back('\n');
    sql.append(source.sql);
    if (!sql.ends_with('\n')) {
      sql.push_back('\n');
    }
  }
  return sql;
}

int docker_major_from_image(const std::string_view image) {
  const auto colon = image.rfind(':');
  if (colon == std::string_view::npos) {
    return 18;
  }
  const auto tag = image.substr(colon + 1U);
  for (const int major : {15, 16, 17, 18}) {
    const auto text = std::to_string(major);
    if (tag == text || tag.starts_with(text + "-") || tag.starts_with(text + ".")) {
      return major;
    }
  }
  return 18;
}

PostgresProvisioning provision_postgresql(const Config& config, const Runtime& runtime) {
  if (!config.scratch.locator.empty()) {
    const auto locator = resolve_locator(config.scratch.locator, runtime.environment);
    if (!locator) {
      lifecycle_error(ErrorCode::configuration, "PostgreSQL scratch locator is missing");
    }
    return PostgresProvisioning{*locator, std::nullopt};
  }
  if (config.scratch.docker) {
    docker::PostgresContainerOptions options;
    options.image = config.scratch.docker->image;
    options.postgres_major = docker_major_from_image(config.scratch.docker->image);
    auto container = docker::PostgresContainer::create(std::move(options));
    auto locator = container.connection_dsn();
    return PostgresProvisioning{std::move(locator), std::move(container)};
  }
  lifecycle_error(
      ErrorCode::configuration,
      "PostgreSQL schema reconstruction requires scratch.database, scratch.database_env, or "
      "scratch.docker");
}

} // namespace dbdiff::app_detail
