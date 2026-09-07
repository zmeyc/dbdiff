#pragma once
#include "dbdiff/sqlite.hpp"
#include <set>
namespace dbdiff::sqlite::detail {
[[nodiscard]] char ascii_lower(char) noexcept;
[[nodiscard]] std::string ascii_lower(std::string_view);
[[nodiscard]] bool starts_with_reserved_name(std::string_view);
[[nodiscard]] bool is_dbdiff_reserved_name(std::string_view);
[[nodiscard]] bool is_history_table(std::string_view);
[[nodiscard]] bool is_space(char) noexcept;
[[nodiscard]] bool is_word_start(char) noexcept;
[[nodiscard]] bool is_word_continue(char) noexcept;
[[nodiscard]] std::size_t skip_space_and_comments(std::string_view, std::size_t);
[[nodiscard]] bool has_sql(std::string_view);
[[nodiscard]] std::string pragma_name(std::string_view);
[[nodiscard]] std::string canonicalize_sql(std::string_view,
                                           const std::set<std::size_t>& declarations = {});
[[nodiscard]] std::string canonicalize_schema_sql(std::string_view);
[[nodiscard]] std::string_view trim_sql(std::string_view);
[[nodiscard]] std::string quote_identifier(std::string_view);
[[nodiscard]] std::string quote_literal(std::string_view);
[[nodiscard]] std::string terminated_statement(std::string_view);
[[nodiscard]] std::size_t skip_quoted_schema_token(std::string_view, std::size_t);
[[nodiscard]] bool contains_unquoted_word(std::string_view, std::string_view);
void validate_source_statements(std::string_view, const std::vector<StatementSpan>&);
void validate_migration_statements(std::string_view, const std::vector<StatementSpan>&);
struct TableDefinition {
  std::size_t open_parenthesis{0};
  std::size_t close_parenthesis{0};
  std::vector<std::string> columns;
  std::vector<std::string> constraints;
  std::string trailing;
};

[[nodiscard]] TableDefinition parse_table_definition(const TableSnapshot&);
} // namespace dbdiff::sqlite::detail
