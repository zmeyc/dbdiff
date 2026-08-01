#pragma once

#include <string_view>

namespace dbdiff {

[[nodiscard]] std::string_view version() noexcept;

} // namespace dbdiff
