#include "dbdiff/migration.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"
#include "dbdiff/source.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace dbdiff {
namespace {

[[noreturn]] void migration_error(const std::string& message) {
  throw Error{ErrorCode::migration, message};
}

bool is_lower_hex_hash(const std::string_view value) {
  if (value.size() != 64) {
    return false;
  }
  return value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string read_file(const std::filesystem::path& file) {
  std::ifstream input{file, std::ios::binary};
  if (!input) {
    migration_error("cannot open migration '" + file.string() + "'");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    migration_error("cannot read migration '" + file.string() + "'");
  }
  auto sql = contents.str();
  if (sql.find('\0') != std::string::npos) {
    migration_error("migration contains a NUL byte: '" + file.string() + "'");
  }
  if (!is_valid_utf8(sql)) {
    migration_error("migration is not valid UTF-8: '" + file.string() + "'");
  }
  return sql;
}

BackendKind parse_backend(const std::string_view value) {
  if (value == "postgresql") {
    return BackendKind::postgresql;
  }
  if (value == "sqlite") {
    return BackendKind::sqlite;
  }
  migration_error("migration metadata backend must be 'postgresql' or 'sqlite'");
}

bool parse_boolean(const std::string_view value, const std::string_view key) {
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  migration_error("migration metadata '" + std::string{key} + "' must be true or false");
}

void require_hash(const std::string& value, const std::string_view key) {
  if (!is_lower_hex_hash(value)) {
    migration_error("migration metadata '" + std::string{key} +
                    "' must be a lowercase SHA-256 digest");
  }
}

std::string required_value(const std::map<std::string, std::string, std::less<>>& values,
                           const std::string_view key) {
  const auto found = values.find(key);
  if (found == values.end() || found->second.empty()) {
    migration_error("migration metadata is missing '" + std::string{key} + "'");
  }
  return found->second;
}

std::string random_suffix() {
  std::random_device device;
  std::uniform_int_distribution<unsigned int> distribution{0, 15};
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string result;
  result.reserve(24);
  for (std::size_t index = 0; index < 24; ++index) {
    result.push_back(digits[distribution(device)]);
  }
  return result;
}

std::string system_error_message(const int value) {
  return std::error_code{value, std::generic_category()}.message();
}

class OwnedTemporaryFile {
public:
  OwnedTemporaryFile(std::filesystem::path path, const int descriptor)
      : path_{std::move(path)}, descriptor_{descriptor} {}

  OwnedTemporaryFile(const OwnedTemporaryFile&) = delete;
  OwnedTemporaryFile& operator=(const OwnedTemporaryFile&) = delete;

  ~OwnedTemporaryFile() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
    if (!path_.empty()) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

  void close_checked() {
    if (descriptor_ >= 0 && ::close(descriptor_) != 0) {
      descriptor_ = -1;
      migration_error("cannot close temporary migration file: " + system_error_message(errno));
    }
    descriptor_ = -1;
  }

  void release_path() noexcept { path_.clear(); }

private:
  std::filesystem::path path_;
  int descriptor_;
};

void write_all(const int descriptor, const std::string_view contents) {
  std::size_t written = 0;
  while (written < contents.size()) {
    const auto count = ::write(descriptor, contents.data() + written, contents.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      migration_error("cannot write temporary migration file: " + system_error_message(errno));
    }
    written += static_cast<std::size_t>(count);
  }
}

void sync_directory(const std::filesystem::path& directory) {
#ifdef O_DIRECTORY
  const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
  const auto descriptor = ::open(directory.c_str(), O_RDONLY);
#endif
  if (descriptor < 0) {
    migration_error("cannot open migration directory for sync: " + system_error_message(errno));
  }
  const auto sync_result = ::fsync(descriptor);
  const auto sync_error = errno;
  const auto close_result = ::close(descriptor);
  if (sync_result != 0) {
    migration_error("cannot sync migration directory: " + system_error_message(sync_error));
  }
  if (close_result != 0) {
    migration_error("cannot close migration directory: " + system_error_message(errno));
  }
}

} // namespace

void validate_engine_version(const BackendKind backend, const std::string_view version) {
  if (backend == BackendKind::postgresql) {
    int number = 0;
    const auto [end, error] =
        std::from_chars(version.data(), version.data() + version.size(), number);
    if (error != std::errc{} || end != version.data() + version.size() || number < 150000 ||
        number >= 190000) {
      migration_error("PostgreSQL migration engine version must identify major 15 through 18");
    }
    return;
  }

  std::array<int, 3> components{};
  std::size_t begin = 0U;
  for (std::size_t index = 0U; index < components.size(); ++index) {
    const auto end_position =
        index + 1U == components.size() ? version.size() : version.find('.', begin);
    if (end_position == std::string_view::npos || end_position == begin) {
      migration_error("SQLite migration engine version must use major.minor.patch");
    }
    const auto component = version.substr(begin, end_position - begin);
    const auto [end, error] =
        std::from_chars(component.data(), component.data() + component.size(), components[index]);
    if (error != std::errc{} || end != component.data() + component.size() ||
        components[index] < 0 || components[index] > 999) {
      migration_error("SQLite migration engine version must use numeric major.minor.patch");
    }
    begin = end_position + 1U;
  }
  const auto encoded = components[0] * 1'000'000 + components[1] * 1'000 + components[2];
  if (encoded < 3'045'000) {
    migration_error("SQLite migration engine version must be 3.45.0 or newer");
  }
}

MigrationMetadata parse_migration_metadata(const std::string_view sql) {
  constexpr std::string_view prefix{"-- dbdiff: "};
  std::map<std::string, std::string, std::less<>> values;
  HazardSet hazards;
  std::size_t position = 0;
  std::size_t header_lines = 0;
  while (position < sql.size()) {
    const auto line_end = sql.find('\n', position);
    const auto raw_end = line_end == std::string_view::npos ? sql.size() : line_end;
    auto line = sql.substr(position, raw_end - position);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (!line.starts_with(prefix)) {
      break;
    }
    ++header_lines;
    const auto directive = line.substr(prefix.size());
    const auto separator = directive.find('=');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == directive.size()) {
      migration_error("invalid migration metadata directive on line " +
                      std::to_string(header_lines));
    }
    const auto key = std::string{directive.substr(0, separator)};
    const auto value = std::string{directive.substr(separator + 1)};
    if (key == "allow") {
      const auto hazard = parse_hazard(value);
      if (!hazard) {
        migration_error("unknown migration hazard '" + value + "'");
      }
      if (!hazards.insert(*hazard).second) {
        migration_error("duplicate migration hazard '" + value + "'");
      }
    } else {
      static const std::set<std::string, std::less<>> allowed{
          "format",      "backend",   "version",           "engine-version",
          "from-sha256", "to-sha256", "source-set-sha256", "draft"};
      if (!allowed.contains(key)) {
        migration_error("unknown migration metadata key '" + key + "'");
      }
      if (!values.emplace(key, value).second) {
        migration_error("duplicate migration metadata key '" + key + "'");
      }
    }
    position = line_end == std::string_view::npos ? sql.size() : line_end + 1;
  }
  if (header_lines == 0) {
    migration_error("migration must begin with dbdiff metadata");
  }

  const auto format_text = required_value(values, "format");
  int format = 0;
  const auto [end, conversion_error] =
      std::from_chars(format_text.data(), format_text.data() + format_text.size(), format);
  if (conversion_error != std::errc{} || end != format_text.data() + format_text.size() ||
      format != 1) {
    migration_error("migration metadata format must be 1");
  }

  MigrationMetadata metadata;
  metadata.format = format;
  metadata.backend = parse_backend(required_value(values, "backend"));
  metadata.version = required_value(values, "version");
  metadata.engine_version = required_value(values, "engine-version");
  validate_engine_version(metadata.backend, metadata.engine_version);
  metadata.from_sha256 = required_value(values, "from-sha256");
  metadata.to_sha256 = required_value(values, "to-sha256");
  metadata.source_set_sha256 = required_value(values, "source-set-sha256");
  metadata.allowed_hazards = std::move(hazards);
  if (const auto found = values.find("draft"); found != values.end()) {
    metadata.draft = parse_boolean(found->second, "draft");
  }
  require_hash(metadata.from_sha256, "from-sha256");
  require_hash(metadata.to_sha256, "to-sha256");
  require_hash(metadata.source_set_sha256, "source-set-sha256");
  static_cast<void>(validate_migration_filename(metadata.version + ".sql"));
  return metadata;
}

std::string render_migration_metadata(const MigrationMetadata& metadata) {
  if (metadata.format != 1) {
    migration_error("migration metadata format must be 1");
  }
  if (metadata.version.empty() || metadata.engine_version.empty()) {
    migration_error("migration metadata version and engine version must not be empty");
  }
  validate_engine_version(metadata.backend, metadata.engine_version);
  static_cast<void>(validate_migration_filename(metadata.version + ".sql"));
  require_hash(metadata.from_sha256, "from-sha256");
  require_hash(metadata.to_sha256, "to-sha256");
  require_hash(metadata.source_set_sha256, "source-set-sha256");

  std::ostringstream output;
  output << "-- dbdiff: format=1\n";
  output << "-- dbdiff: backend=" << backend_name(metadata.backend) << '\n';
  output << "-- dbdiff: version=" << metadata.version << '\n';
  output << "-- dbdiff: engine-version=" << metadata.engine_version << '\n';
  output << "-- dbdiff: from-sha256=" << metadata.from_sha256 << '\n';
  output << "-- dbdiff: to-sha256=" << metadata.to_sha256 << '\n';
  output << "-- dbdiff: source-set-sha256=" << metadata.source_set_sha256 << '\n';
  for (const auto hazard : metadata.allowed_hazards) {
    output << "-- dbdiff: allow=" << hazard_name(hazard) << '\n';
  }
  output << "-- dbdiff: draft=" << (metadata.draft ? "true" : "false") << "\n\n";
  return output.str();
}

std::string validate_migration_filename(const std::filesystem::path& file) {
  if (file.has_parent_path() && file.parent_path() != ".") {
    migration_error("migration filename must not contain a directory");
  }
  const auto name = file.filename().string();
  static const std::regex valid{R"(^([0-9]{14}_[a-z0-9]+(?:_[a-z0-9]+)*)\.sql$)"};
  std::smatch match;
  if (!std::regex_match(name, match, valid)) {
    migration_error("migration filename must match YYYYMMDDHHMMSS_slug.sql");
  }
  const auto version = match[1].str();
  const auto timestamp = std::string_view{version}.substr(0, 14);
  const auto parse_field = [&](const std::size_t offset, const std::size_t size) {
    int value = 0;
    const auto field = timestamp.substr(offset, size);
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (error != std::errc{} || end != field.data() + field.size()) {
      migration_error("migration filename contains an invalid UTC timestamp");
    }
    return value;
  };
  const auto year_value = parse_field(0, 4);
  const auto month_value = parse_field(4, 2);
  const auto day_value = parse_field(6, 2);
  const auto hour_value = parse_field(8, 2);
  const auto minute_value = parse_field(10, 2);
  const auto second_value = parse_field(12, 2);
  const std::chrono::year_month_day date{std::chrono::year{year_value},
                                         std::chrono::month{static_cast<unsigned>(month_value)},
                                         std::chrono::day{static_cast<unsigned>(day_value)}};
  if (year_value < 1 || year_value > 9999 || !date.ok() || hour_value > 23 || minute_value > 59 ||
      second_value > 59) {
    migration_error("migration filename contains an invalid UTC timestamp");
  }
  return version;
}

MigrationFile load_migration(const std::filesystem::path& file,
                             const BackendKind expected_backend) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(file, error);
  if (error || !std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
    migration_error("migration is not a regular non-symlink file: '" + file.string() + "'");
  }
  const auto version = validate_migration_filename(file.filename());
  auto sql = read_file(file);
  auto metadata = parse_migration_metadata(sql);
  if (metadata.version != version) {
    migration_error("migration metadata version does not match filename: '" + file.string() + "'");
  }
  if (metadata.backend != expected_backend) {
    migration_error("migration backend does not match the selected backend: '" + file.string() +
                    "'");
  }
  const auto exact_hash = sha256_hex(sql);
  return MigrationFile{file, std::move(metadata), std::move(sql), exact_hash};
}

std::vector<MigrationFile> load_migrations(const std::filesystem::path& directory,
                                           const BackendKind expected_backend) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(directory, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    migration_error("cannot inspect migration directory '" + directory.string() +
                    "': " + error.message());
  }
  if (!std::filesystem::exists(status)) {
    return {};
  }
  if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
    migration_error("migration path must be a non-symlink directory: '" + directory.string() + "'");
  }

  std::vector<std::filesystem::path> files;
  for (std::filesystem::directory_iterator iterator{directory, error}, end; iterator != end;
       iterator.increment(error)) {
    if (error) {
      migration_error("cannot enumerate migration directory '" + directory.string() +
                      "': " + error.message());
    }
    const auto entry_status = iterator->symlink_status(error);
    if (error) {
      migration_error("cannot inspect migration entry '" + iterator->path().string() +
                      "': " + error.message());
    }
    if (std::filesystem::is_symlink(entry_status)) {
      migration_error("symbolic links are not allowed in migration directories: '" +
                      iterator->path().string() + "'");
    }
    if (std::filesystem::is_regular_file(entry_status) && iterator->path().extension() == ".sql") {
      files.emplace_back(iterator->path());
    }
  }
  std::ranges::sort(files, [](const auto& left, const auto& right) {
    return left.filename().generic_string() < right.filename().generic_string();
  });

  std::vector<MigrationFile> migrations;
  migrations.reserve(files.size());
  for (const auto& file : files) {
    migrations.emplace_back(load_migration(file, expected_backend));
  }
  return migrations;
}

void save_migration_atomic(const std::filesystem::path& directory, const std::string_view version,
                           const std::string_view sql) {
  static_cast<void>(validate_migration_filename(std::string{version} + ".sql"));
  if (sql.find('\0') != std::string_view::npos || !is_valid_utf8(sql)) {
    migration_error("migration SQL must be valid UTF-8 without NUL bytes");
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    migration_error("cannot create migration directory '" + directory.string() +
                    "': " + filesystem_error.message());
  }
  const auto directory_status = std::filesystem::symlink_status(directory, filesystem_error);
  if (filesystem_error || std::filesystem::is_symlink(directory_status) ||
      !std::filesystem::is_directory(directory_status)) {
    migration_error("migration path must be a non-symlink directory: '" + directory.string() + "'");
  }

  const auto final_path = directory / (std::string{version} + ".sql");
  const auto temporary_path = directory / ("." + std::string{version} + ".tmp-" + random_suffix());
  const auto descriptor =
      ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP);
  if (descriptor < 0) {
    migration_error("cannot create temporary migration file: " + system_error_message(errno));
  }
  OwnedTemporaryFile temporary{temporary_path, descriptor};
  write_all(temporary.descriptor(), sql);
  if (::fsync(temporary.descriptor()) != 0) {
    migration_error("cannot sync temporary migration file: " + system_error_message(errno));
  }
  temporary.close_checked();

  if (::link(temporary_path.c_str(), final_path.c_str()) != 0) {
    if (errno == EEXIST) {
      migration_error("migration already exists: '" + final_path.string() + "'");
    }
    migration_error("cannot install migration '" + final_path.string() +
                    "': " + system_error_message(errno));
  }
  if (::unlink(temporary_path.c_str()) != 0) {
    migration_error("migration was saved but its temporary link could not be removed: '" +
                    temporary_path.string() + "'");
  }
  temporary.release_path();
  sync_directory(directory);
}

} // namespace dbdiff
