#pragma once

#include <filesystem>
#include <functional>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff {

struct ResolvedSource {
  std::string display_path;
  std::filesystem::path absolute_path;
  std::string sql;
};

struct SourceSet {
  std::vector<ResolvedSource> files;
  std::string exact_sha256;
};

using StdinReader = std::function<std::string()>;

class SourceResolver {
public:
  SourceResolver(const std::filesystem::path& base_directory,
                 const std::filesystem::path& migrations_directory, StdinReader stdin_reader);

  [[nodiscard]] SourceSet resolve(const std::vector<std::filesystem::path>& entries) const;

private:
  std::filesystem::path base_directory_;
  std::filesystem::path migrations_directory_;
  StdinReader stdin_reader_;
};

[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;

} // namespace dbdiff
