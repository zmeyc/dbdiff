#pragma once

#include <string_view>

namespace dbdiff {

enum class BackendKind { postgresql, sqlite };

[[nodiscard]] std::string_view backend_name(BackendKind backend) noexcept;

} // namespace dbdiff
