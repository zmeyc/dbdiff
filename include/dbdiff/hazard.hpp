#pragma once

#include <optional>
#include <set>
#include <string_view>

namespace dbdiff {

enum class Hazard {
  data_loss,
  data_migration_required,
  table_rewrite,
  write_lock,
  constraint_scan,
  nontransactional,
  enum_rebuild,
  materialized_view_rebuild,
  breaking_routine_change,
  untrackable_routine_dependency,
  rowid_reassignment,
};

using HazardSet = std::set<Hazard>;

[[nodiscard]] std::string_view hazard_name(Hazard hazard) noexcept;
[[nodiscard]] std::optional<Hazard> parse_hazard(std::string_view name) noexcept;

} // namespace dbdiff
