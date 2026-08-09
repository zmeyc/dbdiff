#include "dbdiff/config.hpp"

#include "dbdiff/error.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dbdiff {
namespace {

[[noreturn]] void config_error(const std::filesystem::path& file, const std::string& message) {
  throw Error{ErrorCode::configuration, file.string() + ": " + message};
}

std::string read_file(const std::filesystem::path& file) {
  std::ifstream input{file, std::ios::binary};
  if (!input) {
    config_error(file, "cannot open configuration file");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    config_error(file, "cannot read configuration file");
  }
  return contents.str();
}

std::string trim_copy(const std::string_view input) {
  const auto first = std::find_if_not(input.begin(), input.end(), [](const unsigned char value) {
    return std::isspace(value) != 0;
  });
  const auto last = std::find_if_not(input.rbegin(), input.rend(), [](const unsigned char value) {
                      return std::isspace(value) != 0;
                    }).base();
  if (first >= last) {
    return {};
  }
  return {first, last};
}

void reject_advanced_yaml(const std::filesystem::path& file, const std::string& contents) {
  const std::regex anchor_or_alias{R"((^|[\s\[\]{},])([&*])[A-Za-z0-9_-]+)"};
  const std::regex custom_tag{R"((^|[\s\[\]{},])![A-Za-z0-9_-]+)"};

  std::istringstream lines{contents};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(lines, line)) {
    ++line_number;
    const auto trimmed = trim_copy(line);
    if (trimmed == "---" || trimmed == "..." || trimmed.starts_with("<<:")) {
      config_error(file, "document markers and merge keys are not supported (line " +
                             std::to_string(line_number) + ")");
    }
    if (std::regex_search(line, anchor_or_alias) || std::regex_search(line, custom_tag)) {
      config_error(file, "YAML aliases, anchors, and custom tags are not supported (line " +
                             std::to_string(line_number) + ")");
    }
  }
}

void require_mapping(const std::filesystem::path& file, const YAML::Node& node,
                     const std::string_view context) {
  if (!node || !node.IsMap()) {
    config_error(file, std::string{context} + " must be a mapping");
  }
}

void validate_keys(const std::filesystem::path& file, const YAML::Node& node,
                   const std::set<std::string, std::less<>>& allowed,
                   const std::string_view context) {
  require_mapping(file, node, context);
  std::set<std::string, std::less<>> seen;
  for (const auto& item : node) {
    if (!item.first.IsScalar()) {
      config_error(file, std::string{context} + " contains a non-scalar key");
    }
    const auto key = item.first.as<std::string>();
    if (!seen.insert(key).second) {
      config_error(file, std::string{context} + " contains duplicate key '" + key + "'");
    }
    if (!allowed.contains(key)) {
      config_error(file, std::string{context} + " contains unknown key '" + key + "'");
    }
  }
}

std::string required_scalar(const std::filesystem::path& file, const YAML::Node& node,
                            const std::string_view key) {
  const auto value = node[std::string{key}];
  if (!value || !value.IsScalar()) {
    config_error(file, "'" + std::string{key} + "' must be a non-empty string");
  }
  auto result = value.as<std::string>();
  if (result.empty()) {
    config_error(file, "'" + std::string{key} + "' must be a non-empty string");
  }
  return result;
}

std::optional<std::string> optional_scalar(const std::filesystem::path& file,
                                           const YAML::Node& node, const std::string_view key) {
  const auto value = node[std::string{key}];
  if (!value) {
    return std::nullopt;
  }
  if (!value.IsScalar()) {
    config_error(file, "'" + std::string{key} + "' must be a string");
  }
  auto result = value.as<std::string>();
  if (result.empty()) {
    config_error(file, "'" + std::string{key} + "' must not be empty");
  }
  return result;
}

void validate_environment_name(const std::filesystem::path& file,
                               const std::optional<std::string>& name) {
  if (!name) {
    return;
  }
  const std::regex valid{R"([A-Za-z_][A-Za-z0-9_]*)"};
  if (!std::regex_match(*name, valid)) {
    config_error(file, "environment variable name '" + *name + "' is invalid");
  }
}

LocatorConfig parse_locator(const std::filesystem::path& file, const YAML::Node& node,
                            const std::string_view literal_key,
                            const std::string_view environment_key) {
  LocatorConfig locator{optional_scalar(file, node, literal_key),
                        optional_scalar(file, node, environment_key)};
  if (locator.literal && locator.environment_variable) {
    config_error(file, "'" + std::string{literal_key} + "' and '" + std::string{environment_key} +
                           "' are mutually exclusive");
  }
  validate_environment_name(file, locator.environment_variable);
  return locator;
}

std::chrono::milliseconds parse_duration(const std::filesystem::path& file, const YAML::Node& node,
                                         const std::string_view key,
                                         const std::chrono::milliseconds default_value) {
  const auto value = node[std::string{key}];
  if (!value) {
    return default_value;
  }
  if (!value.IsScalar()) {
    config_error(file, "'" + std::string{key} + "' must be a duration");
  }
  const auto text = value.as<std::string>();
  const auto suffix_begin = text.find_first_not_of("0123456789");
  if (suffix_begin == 0 || suffix_begin == std::string::npos) {
    config_error(file, "'" + std::string{key} + "' must use ms, s, m, or h");
  }

  std::uint64_t number = 0;
  const auto number_text = std::string_view{text}.substr(0, suffix_begin);
  const auto [end, error] =
      std::from_chars(number_text.data(), number_text.data() + number_text.size(), number);
  if (error != std::errc{} || end != number_text.data() + number_text.size()) {
    config_error(file, "'" + std::string{key} + "' has an invalid duration");
  }

  const auto suffix = std::string_view{text}.substr(suffix_begin);
  std::uint64_t multiplier = 0;
  if (suffix == "ms") {
    multiplier = 1;
  } else if (suffix == "s") {
    multiplier = 1000;
  } else if (suffix == "m") {
    multiplier = 60'000;
  } else if (suffix == "h") {
    multiplier = 3'600'000;
  } else {
    config_error(file, "'" + std::string{key} + "' must use ms, s, m, or h");
  }

  if (number == 0 ||
      number > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) / multiplier) {
    config_error(file, "'" + std::string{key} + "' is outside the supported range");
  }
  return std::chrono::milliseconds{static_cast<std::int64_t>(number * multiplier)};
}

std::vector<std::string> parse_string_list(const std::filesystem::path& file,
                                           const YAML::Node& node, const std::string_view key,
                                           const bool required) {
  const auto value = node[std::string{key}];
  if (!value) {
    if (required) {
      config_error(file, "'" + std::string{key} + "' is required");
    }
    return {};
  }
  if (!value.IsSequence() || (required && value.size() == 0)) {
    config_error(file, "'" + std::string{key} + "' must be a non-empty list");
  }

  std::vector<std::string> values;
  values.reserve(value.size());
  for (const auto& entry : value) {
    if (!entry.IsScalar()) {
      config_error(file, "'" + std::string{key} + "' must contain only strings");
    }
    auto text = entry.as<std::string>();
    if (text.empty()) {
      config_error(file, "'" + std::string{key} + "' contains an empty value");
    }
    values.emplace_back(std::move(text));
  }
  return values;
}

std::filesystem::path resolve_path(const std::filesystem::path& base,
                                   const std::filesystem::path& path) {
  return path.is_absolute() ? path.lexically_normal() : (base / path).lexically_normal();
}

BackendKind parse_backend(const std::filesystem::path& file, const std::string_view value) {
  if (value == "postgresql") {
    return BackendKind::postgresql;
  }
  if (value == "sqlite") {
    return BackendKind::sqlite;
  }
  config_error(file, "backend must be 'postgresql' or 'sqlite'");
}

std::string lower_copy(const std::string_view value) {
  std::string result;
  result.reserve(value.size());
  std::ranges::transform(value, std::back_inserter(result), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

} // namespace

bool LocatorConfig::empty() const noexcept { return !literal && !environment_variable; }

bool ScratchConfig::empty() const noexcept { return locator.empty() && !docker; }

Config load_config(const std::filesystem::path& input_file) {
  std::error_code error;
  const auto file = std::filesystem::absolute(input_file, error).lexically_normal();
  if (error) {
    config_error(input_file, "cannot resolve configuration path: " + error.message());
  }

  const auto contents = read_file(file);
  reject_advanced_yaml(file, contents);

  YAML::Node root;
  try {
    root = YAML::Load(contents);
  } catch (const YAML::Exception& yaml_error) {
    config_error(file, yaml_error.what());
  }

  validate_keys(file, root,
                {"format", "backend", "database", "database_env", "sources", "migrations",
                 "scratch", "managed_schemas", "lock_timeout", "statement_timeout"},
                "configuration");

  const auto format_node = root["format"];
  int format = 0;
  try {
    format = format_node && format_node.IsScalar() ? format_node.as<int>() : 0;
  } catch (const YAML::Exception&) {
    format = 0;
  }
  if (format != 1) {
    config_error(file, "format must be 1");
  }

  const auto backend = parse_backend(file, required_scalar(file, root, "backend"));
  const auto database = parse_locator(file, root, "database", "database_env");
  if (database.literal) {
    const auto inferred = infer_backend(*database.literal);
    if (inferred && *inferred != backend) {
      config_error(file, "database locator conflicts with configured backend");
    }
  }

  const auto base = file.parent_path();
  auto source_strings = parse_string_list(file, root, "sources", true);
  std::vector<std::filesystem::path> sources;
  sources.reserve(source_strings.size());
  for (const auto& source : source_strings) {
    sources.emplace_back(source == "-" ? std::filesystem::path{"-"} : resolve_path(base, source));
  }

  const auto migrations = resolve_path(base, required_scalar(file, root, "migrations"));

  ScratchConfig scratch;
  if (const auto node = root["scratch"]) {
    validate_keys(file, node, {"database", "database_env", "docker"}, "scratch");
    scratch.locator = parse_locator(file, node, "database", "database_env");
    if (const auto docker = node["docker"]) {
      validate_keys(file, docker, {"image"}, "scratch.docker");
      scratch.docker = DockerScratchConfig{required_scalar(file, docker, "image")};
    }
    if (!scratch.locator.empty() && scratch.docker) {
      config_error(file, "scratch locator and scratch.docker are mutually exclusive");
    }
  }

  auto managed_schemas = parse_string_list(file, root, "managed_schemas", false);
  if (backend == BackendKind::postgresql && managed_schemas.empty()) {
    managed_schemas.emplace_back("public");
  }
  if (backend == BackendKind::sqlite && !managed_schemas.empty()) {
    config_error(file, "managed_schemas is only valid for PostgreSQL");
  }

  return Config{file,
                backend,
                database,
                std::move(sources),
                migrations,
                std::move(scratch),
                std::move(managed_schemas),
                parse_duration(file, root, "lock_timeout", std::chrono::seconds{5}),
                parse_duration(file, root, "statement_timeout", std::chrono::minutes{5})};
}

std::optional<std::filesystem::path> discover_config(const std::filesystem::path& start) {
  std::error_code error;
  auto directory = std::filesystem::absolute(start, error).lexically_normal();
  if (error) {
    throw Error{ErrorCode::configuration,
                "cannot resolve configuration search path: " + error.message()};
  }
  if (std::filesystem::is_regular_file(directory, error)) {
    directory = directory.parent_path();
  }

  auto git_root = std::optional<std::filesystem::path>{};
  for (auto candidate = directory;; candidate = candidate.parent_path()) {
    if (std::filesystem::exists(candidate / ".git", error) && !error) {
      git_root = candidate;
      break;
    }
    if (candidate == candidate.root_path()) {
      break;
    }
  }

  for (auto candidate = directory;; candidate = candidate.parent_path()) {
    auto config = candidate / "dbdiff.yaml";
    if (std::filesystem::is_regular_file(config, error) && !error) {
      return config;
    }
    if ((git_root && candidate == *git_root) || candidate == candidate.root_path()) {
      break;
    }
  }
  return std::nullopt;
}

std::optional<BackendKind> infer_backend(const std::string_view locator) {
  const auto lowered = lower_copy(locator);
  if (lowered.starts_with("postgresql:") || lowered.starts_with("postgres:")) {
    return BackendKind::postgresql;
  }
  if (lowered.starts_with("sqlite:")) {
    return BackendKind::sqlite;
  }
  return std::nullopt;
}

std::optional<std::string> resolve_locator(const LocatorConfig& locator,
                                           const EnvironmentLookup& environment) {
  if (locator.literal) {
    return locator.literal;
  }
  if (!locator.environment_variable) {
    return std::nullopt;
  }
  const auto value = environment(*locator.environment_variable);
  if (!value || value->empty()) {
    throw Error{ErrorCode::configuration,
                "environment variable '" + *locator.environment_variable + "' is not set"};
  }
  return value;
}

std::string redact_locator(const std::string_view locator) {
  if (infer_backend(locator) == BackendKind::sqlite) {
    return std::string{locator};
  }

  auto result = std::string{locator};
  if (const auto scheme = result.find("://"); scheme != std::string::npos) {
    const auto authority_begin = scheme + 3;
    const auto authority_end = result.find_first_of("/?#", authority_begin);
    const auto at = result.rfind('@', authority_end);
    if (at != std::string::npos && at >= authority_begin) {
      const auto colon = result.find(':', authority_begin);
      if (colon != std::string::npos && colon < at) {
        result.replace(colon + 1, at - colon - 1, "***");
      }
    }
    return result;
  }

  const std::regex secret{R"((^|\s)(password|passfile)\s*=\s*('[^']*'|\S+))", std::regex::icase};
  return std::regex_replace(result, secret, "$1$2=***");
}

} // namespace dbdiff
