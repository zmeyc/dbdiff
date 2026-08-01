#pragma once

#include "dbdiff/hazard.hpp"

#include <string>
#include <vector>

namespace dbdiff {

enum class TransactionMode { required, allowed, forbidden };

struct Operation {
  std::string id;
  std::string object_identity;
  std::vector<std::string> dependencies;
  TransactionMode transaction_mode{TransactionMode::allowed};
  std::string sql;
  HazardSet hazards;
  std::string expected_before;
  std::string expected_after;
};

[[nodiscard]] std::vector<std::size_t>
deterministic_operation_order(const std::vector<Operation>& operations);

} // namespace dbdiff
