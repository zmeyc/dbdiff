#include "dbdiff/source.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dbdiff {
namespace {

[[noreturn]] void source_error(const std::string& message) {
  throw Error{ErrorCode::source, message};
}

std::filesystem::path absolute_normal(const std::filesystem::path& path) {
  std::error_code error;
  auto result = std::filesystem::absolute(path, error).lexically_normal();
  if (error) {
    source_error("cannot resolve path '" + path.string() + "': " + error.message());
  }
  return result;
}

std::filesystem::path canonical_existing(const std::filesystem::path& path) {
  std::error_code error;
  const auto result = std::filesystem::canonical(path, error);
  if (error) {
    source_error("cannot resolve source '" + path.string() + "': " + error.message());
  }
  return result;
}

std::filesystem::path canonical_allow_missing(const std::filesystem::path& path) {
  std::error_code error;
  const auto result = std::filesystem::weakly_canonical(path, error);
  if (error) {
    source_error("cannot normalize path '" + path.string() + "': " + error.message());
  }
  return result;
}

bool path_is_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
  const auto relative = child.lexically_relative(parent);
  if (relative.empty()) {
    return child == parent;
  }
  const auto first = *relative.begin();
  return first != ".." && !relative.is_absolute();
}

bool paths_overlap(const std::filesystem::path& first, const std::filesystem::path& second) {
  return path_is_within(first, second) || path_is_within(second, first);
}

bool has_symlink_component(const std::filesystem::path& path, const std::filesystem::path& base) {
  auto current = base;
  auto relative = path.lexically_relative(base);
  if (relative.empty() || (!relative.empty() && *relative.begin() == "..")) {
    relative = path.filename();
    current = path.parent_path();
  }
  for (const auto& component : relative) {
    current /= component;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(current, error);
    if (!error && std::filesystem::is_symlink(status)) {
      return true;
    }
  }
  return false;
}

std::string read_sql(const std::filesystem::path& file) {
  std::ifstream input{file, std::ios::binary};
  if (!input) {
    source_error("cannot open source file '" + file.string() + "'");
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof()) {
    source_error("cannot read source file '" + file.string() + "'");
  }
  auto sql = contents.str();
  if (sql.find('\0') != std::string::npos) {
    source_error("source file contains NUL byte: '" + file.string() + "'");
  }
  if (!is_valid_utf8(sql)) {
    source_error("source file is not valid UTF-8: '" + file.string() + "'");
  }
  return sql;
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

bool is_manifest(const std::filesystem::path& path) {
  return path.filename() == "dbdiff.schema" || path.extension() == ".dbdiff-schema";
}

bool has_parent_component(const std::filesystem::path& path) {
  return std::ranges::any_of(path, [](const auto& component) { return component == ".."; });
}

bool has_glob(const std::string_view value) {
  return value.find_first_of("*?[]{}") != std::string_view::npos;
}

std::string display_path(const std::filesystem::path& file, const std::filesystem::path& base) {
  const auto relative = file.lexically_relative(base);
  if (!relative.empty() && !has_parent_component(relative)) {
    return relative.generic_string();
  }
  return file.generic_string();
}

class Resolution {
public:
  Resolution(std::filesystem::path base, std::filesystem::path migrations, StdinReader stdin_reader)
      : base_{std::move(base)}, migrations_{std::move(migrations)},
        stdin_reader_{std::move(stdin_reader)} {}

  SourceSet run(const std::vector<std::filesystem::path>& entries) {
    if (entries.empty()) {
      source_error("at least one schema source is required");
    }
    for (const auto& entry : entries) {
      if (entry == "-") {
        add_stdin();
      } else {
        process(absolute_normal(entry.is_absolute() ? entry : base_ / entry));
      }
    }
    if (files_.empty()) {
      source_error("schema sources resolved to no SQL files");
    }

    Sha256 digest;
    digest.add("dbdiff-source-set-v1");
    for (const auto& file : files_) {
      digest.add_length_prefixed(file.display_path);
      digest.add_length_prefixed(file.sql);
    }
    return SourceSet{std::move(files_), digest.finish_hex()};
  }

private:
  void process(const std::filesystem::path& path) {
    if (has_symlink_component(path, base_)) {
      source_error("symbolic links are not allowed in schema sources: '" + path.string() + "'");
    }

    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::exists(status)) {
      source_error("schema source does not exist: '" + path.string() + "'");
    }
    if (std::filesystem::is_directory(status)) {
      process_directory(path);
      return;
    }
    if (!std::filesystem::is_regular_file(status)) {
      source_error("schema source is not a regular file or directory: '" + path.string() + "'");
    }
    if (is_manifest(path)) {
      process_manifest(path);
      return;
    }
    if (path.extension() != ".sql") {
      source_error("schema source must be SQL or a dbdiff manifest: '" + path.string() + "'");
    }
    add_file(path);
  }

  void process_directory(const std::filesystem::path& directory) {
    const auto canonical_directory = canonical_existing(directory);
    if (paths_overlap(canonical_directory, migrations_)) {
      source_error("schema source directory overlaps the migrations directory: '" +
                   directory.string() + "'");
    }

    const auto manifest = directory / "dbdiff.schema";
    std::error_code error;
    if (std::filesystem::is_regular_file(manifest, error) && !error) {
      process_manifest(manifest);
      return;
    }

    std::vector<std::filesystem::path> sql_files;
    for (std::filesystem::recursive_directory_iterator iterator{directory, error}, end;
         iterator != end; iterator.increment(error)) {
      if (error) {
        source_error("cannot enumerate schema source directory '" + directory.string() +
                     "': " + error.message());
      }
      if (iterator->is_symlink()) {
        source_error("symbolic links are not allowed in schema sources: '" +
                     iterator->path().string() + "'");
      }
      if (iterator->is_regular_file() && iterator->path().extension() == ".sql") {
        sql_files.emplace_back(iterator->path());
      }
    }
    std::ranges::sort(sql_files, [&directory](const auto& left, const auto& right) {
      return left.lexically_relative(directory).generic_string() <
             right.lexically_relative(directory).generic_string();
    });
    for (const auto& file : sql_files) {
      add_file(file);
    }
  }

  void process_manifest(const std::filesystem::path& manifest) {
    const auto canonical_manifest = canonical_existing(manifest);
    const auto key = canonical_manifest.generic_string();
    if (visiting_manifests_.contains(key)) {
      source_error("schema manifest cycle detected at '" + manifest.string() + "'");
    }
    visiting_manifests_.insert(key);

    std::ifstream input{canonical_manifest};
    if (!input) {
      source_error("cannot open schema manifest '" + manifest.string() + "'");
    }
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
      ++line_number;
      if (const auto comment = line.find('#'); comment != std::string::npos) {
        line.erase(comment);
      }
      const auto value = trim_copy(line);
      if (value.empty()) {
        continue;
      }
      const std::filesystem::path entry{value};
      if (entry.is_absolute() || has_parent_component(entry) || has_glob(value)) {
        source_error("invalid manifest entry at '" + manifest.string() + ":" +
                     std::to_string(line_number) + "'");
      }
      process(canonical_manifest.parent_path() / entry);
    }
    if (!input.good() && !input.eof()) {
      source_error("cannot read schema manifest '" + manifest.string() + "'");
    }

    visiting_manifests_.erase(key);
  }

  void add_file(const std::filesystem::path& file) {
    const auto canonical = canonical_existing(file);
    if (path_is_within(canonical, migrations_)) {
      source_error("schema source is inside the migrations directory: '" + file.string() + "'");
    }
    const auto key = canonical.generic_string();
    if (!seen_files_.insert(key).second) {
      source_error("duplicate schema source: '" + file.string() + "'");
    }
    files_.push_back(
        ResolvedSource{display_path(canonical, base_), canonical, read_sql(canonical)});
  }

  void add_stdin() {
    if (stdin_seen_) {
      source_error("stdin may be used as a schema source only once");
    }
    stdin_seen_ = true;
    auto sql = stdin_reader_();
    if (sql.find('\0') != std::string::npos || !is_valid_utf8(sql)) {
      source_error("stdin schema source must be valid UTF-8 without NUL bytes");
    }
    files_.push_back(ResolvedSource{"<stdin>", {}, std::move(sql)});
  }

  std::filesystem::path base_;
  std::filesystem::path migrations_;
  StdinReader stdin_reader_;
  std::vector<ResolvedSource> files_;
  std::set<std::string, std::less<>> seen_files_;
  std::set<std::string, std::less<>> visiting_manifests_;
  bool stdin_seen_{false};
};

} // namespace

SourceResolver::SourceResolver(const std::filesystem::path& base_directory,
                               const std::filesystem::path& migrations_directory,
                               StdinReader stdin_reader)
    : base_directory_{canonical_allow_missing(absolute_normal(base_directory))},
      migrations_directory_{canonical_allow_missing(absolute_normal(migrations_directory))},
      stdin_reader_{std::move(stdin_reader)} {
  if (!stdin_reader_) {
    source_error("stdin reader must be configured");
  }
}

SourceSet SourceResolver::resolve(const std::vector<std::filesystem::path>& entries) const {
  return Resolution{base_directory_, migrations_directory_, stdin_reader_}.run(entries);
}

bool is_valid_utf8(const std::string_view value) noexcept {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
      length = 2;
      code_point = first & 0x1fU;
      minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
      length = 3;
      code_point = first & 0x0fU;
      minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
      length = 4;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (index + length > value.size()) {
      return false;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      const auto byte = static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    if (code_point < minimum || code_point > 0x10ffffU ||
        (code_point >= 0xd800U && code_point <= 0xdfffU)) {
      return false;
    }
    index += length;
  }
  return true;
}

} // namespace dbdiff
