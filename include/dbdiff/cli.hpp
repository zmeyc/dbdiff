#pragma once

#include "dbdiff/lifecycle.hpp"

#include <filesystem>
#include <iosfwd>

namespace dbdiff {

[[nodiscard]] int run_cli(int argc, char** argv, std::ostream& output, std::ostream& error,
                          const std::filesystem::path& working_directory,
                          const Runtime& runtime = default_runtime());

} // namespace dbdiff
