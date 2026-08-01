#include "dbdiff/script.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dbdiff {
namespace {

[[noreturn]] void script_error(const std::string& message) {
  throw Error{ErrorCode::migration, message};
}

ExecutionUnit make_unit(const std::string& sql, const std::size_t ordinal,
                        std::vector<StatementSpan> statements, const bool explicit_transaction) {
  const auto begin = statements.front().begin;
  const auto end = statements.back().end;
  return ExecutionUnit{ordinal,
                       begin,
                       end,
                       explicit_transaction,
                       sha256_hex(std::string_view{sql}.substr(begin, end - begin)),
                       std::move(statements)};
}

} // namespace

ParsedScript build_execution_units(std::string sql, std::vector<StatementSpan> statements) {
  std::ranges::sort(statements, {}, &StatementSpan::begin);
  std::size_t previous_end = 0;
  for (const auto& statement : statements) {
    if (statement.begin >= statement.end || statement.end > sql.size()) {
      script_error("statement span is outside the migration SQL");
    }
    if (statement.begin < previous_end) {
      script_error("statement spans overlap");
    }
    previous_end = statement.end;
  }

  std::vector<ExecutionUnit> units;
  std::vector<StatementSpan> transaction;
  bool inside_transaction = false;
  for (const auto& statement : statements) {
    switch (statement.kind) {
    case StatementKind::begin:
      if (inside_transaction) {
        script_error("nested BEGIN is not allowed");
      }
      inside_transaction = true;
      transaction.push_back(statement);
      break;
    case StatementKind::commit:
      if (!inside_transaction) {
        script_error("COMMIT without a matching BEGIN");
      }
      transaction.push_back(statement);
      units.push_back(make_unit(sql, units.size(), std::move(transaction), true));
      transaction.clear();
      inside_transaction = false;
      break;
    case StatementKind::rollback:
      script_error("full ROLLBACK is not allowed in a migration");
    case StatementKind::savepoint:
    case StatementKind::release_savepoint:
    case StatementKind::rollback_to_savepoint:
      if (!inside_transaction) {
        script_error("savepoint control requires an explicit transaction");
      }
      transaction.push_back(statement);
      break;
    case StatementKind::dml:
      if (!inside_transaction) {
        script_error("standalone DML is not resumable; place it inside BEGIN/COMMIT");
      }
      transaction.push_back(statement);
      break;
    case StatementKind::ddl:
    case StatementKind::session:
    case StatementKind::query:
    case StatementKind::unknown:
      if (inside_transaction) {
        transaction.push_back(statement);
      } else {
        units.push_back(make_unit(sql, units.size(), {statement}, false));
      }
      break;
    }
  }
  if (inside_transaction) {
    script_error("migration ends inside an explicit transaction");
  }
  return ParsedScript{std::move(sql), std::move(units)};
}

void validate_completed_prefix(const std::vector<std::string>& completed_hashes,
                               const ParsedScript& candidate) {
  if (completed_hashes.size() > candidate.units.size()) {
    throw Error{ErrorCode::migration,
                "edited migration contains fewer units than its completed prefix"};
  }
  for (std::size_t index = 0; index < completed_hashes.size(); ++index) {
    if (completed_hashes[index] != candidate.units[index].exact_sha256) {
      throw Error{ErrorCode::migration,
                  "edited migration changes completed unit " + std::to_string(index + 1)};
    }
  }
}

} // namespace dbdiff
