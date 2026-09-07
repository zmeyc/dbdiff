#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff::postgresql::detail {

std::vector<std::string> index_key_definitions(std::string_view definition, std::size_t key_count);

} // namespace dbdiff::postgresql::detail
