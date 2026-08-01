#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff {

enum class StatementKind {
  begin,
  commit,
  rollback,
  savepoint,
  release_savepoint,
  rollback_to_savepoint,
  ddl,
  dml,
  session,
  query,
  unknown,
};

struct StatementSpan {
  std::size_t begin{0};
  std::size_t end{0};
  StatementKind kind{StatementKind::unknown};
};

struct ExecutionUnit {
  std::size_t ordinal{0};
  std::size_t begin{0};
  std::size_t end{0};
  bool explicit_transaction{false};
  std::string exact_sha256;
  std::vector<StatementSpan> statements;
};

struct ParsedScript {
  std::string sql;
  std::vector<ExecutionUnit> units;
};

[[nodiscard]] ParsedScript build_execution_units(std::string sql,
                                                 std::vector<StatementSpan> statements);
void validate_completed_prefix(const std::vector<std::string>& completed_hashes,
                               const ParsedScript& candidate);

} // namespace dbdiff
