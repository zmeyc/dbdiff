#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dbdiff::test {

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic<unsigned long long> counter{0};
    const auto unique =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(counter.fetch_add(1));
    path_ = std::filesystem::temp_directory_path() / ("dbdiff-test-" + unique);
    std::filesystem::create_directories(path_);
  }

  ~TempDirectory() {
    std::error_code error;
    if (path_.filename().string().starts_with("dbdiff-test-") &&
        path_.parent_path() == std::filesystem::temp_directory_path()) {
      std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, error);
      error.clear();
      std::filesystem::remove_all(path_, error);
    }
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  std::filesystem::path write(const std::filesystem::path& relative, std::string_view contents) {
    const auto file = path_ / relative;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output{file, std::ios::binary};
    if (!output) {
      throw std::runtime_error{"cannot create test file"};
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    return file;
  }

private:
  std::filesystem::path path_;
};

} // namespace dbdiff::test
