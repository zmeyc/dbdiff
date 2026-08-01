#pragma once

#include "dbdiff/backend.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff {

struct LocatorConfig {
  std::optional<std::string> literal;
  std::optional<std::string> environment_variable;

  [[nodiscard]] bool empty() const noexcept;
};

struct DockerScratchConfig {
  std::string image;
};

struct ScratchConfig {
  LocatorConfig locator;
  std::optional<DockerScratchConfig> docker;

  [[nodiscard]] bool empty() const noexcept;
};

struct Config {
  std::filesystem::path file;
  BackendKind backend;
  LocatorConfig database;
  std::vector<std::filesystem::path> sources;
  std::filesystem::path migrations;
  ScratchConfig scratch;
  std::vector<std::string> managed_schemas;
  std::chrono::milliseconds lock_timeout{5000};
  std::chrono::milliseconds statement_timeout{300000};
};

using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view)>;

[[nodiscard]] Config load_config(const std::filesystem::path& file);
[[nodiscard]] std::optional<std::filesystem::path>
discover_config(const std::filesystem::path& start);
[[nodiscard]] std::optional<BackendKind> infer_backend(std::string_view locator);
[[nodiscard]] std::optional<std::string> resolve_locator(const LocatorConfig& locator,
                                                         const EnvironmentLookup& environment);
[[nodiscard]] std::string redact_locator(std::string_view locator);

} // namespace dbdiff
