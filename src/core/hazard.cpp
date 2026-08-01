#include "dbdiff/hazard.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace dbdiff {
namespace {

using HazardName = std::pair<Hazard, std::string_view>;

constexpr std::array<HazardName, 11> kHazards{{
    {Hazard::data_loss, "DATA_LOSS"},
    {Hazard::data_migration_required, "DATA_MIGRATION_REQUIRED"},
    {Hazard::table_rewrite, "TABLE_REWRITE"},
    {Hazard::write_lock, "WRITE_LOCK"},
    {Hazard::constraint_scan, "CONSTRAINT_SCAN"},
    {Hazard::nontransactional, "NONTRANSACTIONAL"},
    {Hazard::enum_rebuild, "ENUM_REBUILD"},
    {Hazard::materialized_view_rebuild, "MATERIALIZED_VIEW_REBUILD"},
    {Hazard::breaking_routine_change, "BREAKING_ROUTINE_CHANGE"},
    {Hazard::untrackable_routine_dependency, "UNTRACKABLE_ROUTINE_DEPENDENCY"},
    {Hazard::rowid_reassignment, "ROWID_REASSIGNMENT"},
}};

} // namespace

std::string_view hazard_name(const Hazard hazard) noexcept {
  for (const auto& [candidate, name] : kHazards) {
    if (candidate == hazard) {
      return name;
    }
  }
  return "UNKNOWN";
}

std::optional<Hazard> parse_hazard(const std::string_view name) noexcept {
  for (const auto& [candidate, candidate_name] : kHazards) {
    if (candidate_name == name) {
      return candidate;
    }
  }
  return std::nullopt;
}

} // namespace dbdiff
