#include "dbdiff/operation.hpp"

#include "dbdiff/error.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

namespace dbdiff {

std::vector<std::size_t> deterministic_operation_order(const std::vector<Operation>& operations) {
  std::map<std::string, std::size_t, std::less<>> by_id;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (operations[index].id.empty()) {
      throw Error{ErrorCode::migration, "operation ID must not be empty"};
    }
    if (!by_id.emplace(operations[index].id, index).second) {
      throw Error{ErrorCode::migration, "duplicate operation ID '" + operations[index].id + "'"};
    }
  }

  std::vector<std::size_t> incoming(operations.size(), 0);
  std::vector<std::vector<std::size_t>> dependents(operations.size());
  for (std::size_t index = 0; index < operations.size(); ++index) {
    std::set<std::string, std::less<>> unique_dependencies;
    for (const auto& dependency : operations[index].dependencies) {
      if (!unique_dependencies.insert(dependency).second) {
        throw Error{ErrorCode::migration, "operation '" + operations[index].id +
                                              "' repeats dependency '" + dependency + "'"};
      }
      const auto found = by_id.find(dependency);
      if (found == by_id.end()) {
        throw Error{ErrorCode::migration, "operation '" + operations[index].id +
                                              "' has unknown dependency '" + dependency + "'"};
      }
      ++incoming[index];
      dependents[found->second].push_back(index);
    }
  }

  using Ready = std::pair<std::string, std::size_t>;
  std::priority_queue<Ready, std::vector<Ready>, std::greater<>> ready;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (incoming[index] == 0) {
      ready.emplace(operations[index].id, index);
    }
  }

  std::vector<std::size_t> result;
  result.reserve(operations.size());
  while (!ready.empty()) {
    const auto [unused_id, index] = ready.top();
    static_cast<void>(unused_id);
    ready.pop();
    result.push_back(index);
    for (const auto dependent : dependents[index]) {
      --incoming[dependent];
      if (incoming[dependent] == 0) {
        ready.emplace(operations[dependent].id, dependent);
      }
    }
  }

  if (result.size() != operations.size()) {
    std::string cycle;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (incoming[index] != 0) {
        if (!cycle.empty()) {
          cycle += ", ";
        }
        cycle += operations[index].id;
      }
    }
    throw Error{ErrorCode::migration, "operation dependency cycle involves: " + cycle};
  }
  return result;
}

} // namespace dbdiff
