#include "sql.hpp"
#include "errors.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dbdiff::sqlite::detail {
[[nodiscard]] char ascii_lower(const char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

[[nodiscard]] std::string ascii_lower(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(ascii_lower(character));
  }
  return result;
}

[[nodiscard]] bool starts_with_reserved_name(const std::string_view name) {
  const auto key = ascii_lower(name);
  return key.starts_with("sqlite_") || key.starts_with("_dbdiff_");
}

[[nodiscard]] bool is_dbdiff_reserved_name(const std::string_view name) {
  return ascii_lower(name).starts_with("_dbdiff_");
}

[[nodiscard]] bool is_history_table(const std::string_view name) {
  const auto key = ascii_lower(name);
  return key == "_dbdiff_migrations" || key == "_dbdiff_migration_revisions" ||
         key == "_dbdiff_migration_units";
}

[[nodiscard]] bool is_space(const char character) noexcept {
  return std::isspace(static_cast<unsigned char>(character)) != 0;
}

[[nodiscard]] std::size_t skip_space_and_comments(const std::string_view sql,
                                                  std::size_t position) {
  while (position < sql.size()) {
    if (is_space(sql[position])) {
      ++position;
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '-' && sql[position + 1U] == '-') {
      position += 2U;
      while (position < sql.size() && sql[position] != '\n') {
        ++position;
      }
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '/' && sql[position + 1U] == '*') {
      const auto end = sql.find("*/", position + 2U);
      if (end == std::string_view::npos) {
        throw_migration("unterminated block comment in SQLite script");
      }
      position = end + 2U;
      continue;
    }
    break;
  }
  return position;
}

[[nodiscard]] bool has_sql(const std::string_view sql) {
  return skip_space_and_comments(sql, 0) != sql.size();
}

[[nodiscard]] bool is_word_start(const char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return std::isalpha(value) != 0 || character == '_';
}

[[nodiscard]] bool is_word_continue(const char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_' || character == '$';
}

[[nodiscard]] std::vector<std::string> statement_words(const std::string_view sql,
                                                       const std::size_t maximum = 12U) {
  std::vector<std::string> words;
  std::size_t position = 0;
  while (position < sql.size() && words.size() < maximum) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }

    const auto character = sql[position];
    if (is_word_start(character)) {
      const auto begin = position++;
      while (position < sql.size() && is_word_continue(sql[position])) {
        ++position;
      }
      words.push_back(ascii_lower(sql.substr(begin, position - begin)));
      continue;
    }

    if (character == '"' || character == '`' || character == '[') {
      const auto closing = character == '[' ? ']' : character;
      ++position;
      std::string identifier;
      bool closed = false;
      while (position < sql.size()) {
        if (sql[position] == closing) {
          if (character != '[' && position + 1U < sql.size() && sql[position + 1U] == closing) {
            identifier.push_back(closing);
            position += 2U;
            continue;
          }
          ++position;
          closed = true;
          break;
        }
        identifier.push_back(sql[position++]);
      }
      if (!closed) {
        throw_migration("unterminated quoted identifier in SQLite script");
      }
      words.push_back(ascii_lower(identifier));
      continue;
    }

    if (character == '\'') {
      ++position;
      bool closed = false;
      while (position < sql.size()) {
        if (sql[position] == '\'') {
          if (position + 1U < sql.size() && sql[position + 1U] == '\'') {
            position += 2U;
            continue;
          }
          ++position;
          closed = true;
          break;
        }
        ++position;
      }
      if (!closed) {
        throw_migration("unterminated string literal in SQLite script");
      }
      continue;
    }

    ++position;
  }
  return words;
}

[[nodiscard]] bool references_reserved_metadata(const std::string_view sql) {
  const auto words = statement_words(sql, sql.size());
  return std::ranges::any_of(words,
                             [](const std::string& word) { return word.starts_with("_dbdiff_"); });
}

[[nodiscard]] StatementKind classify_statement(const std::string_view sql) {
  const auto words = statement_words(sql, 4U);
  if (words.empty()) {
    return StatementKind::unknown;
  }
  const auto& first = words.front();
  if (first == "begin") {
    return StatementKind::begin;
  }
  if (first == "commit" || first == "end") {
    return StatementKind::commit;
  }
  if (first == "rollback") {
    if (std::ranges::find(words, "to") != words.end()) {
      return StatementKind::rollback_to_savepoint;
    }
    return StatementKind::rollback;
  }
  if (first == "savepoint") {
    return StatementKind::savepoint;
  }
  if (first == "release") {
    return StatementKind::release_savepoint;
  }
  if (first == "create" || first == "alter" || first == "drop") {
    return StatementKind::ddl;
  }
  if (first == "insert" || first == "update" || first == "delete" || first == "replace" ||
      first == "with") {
    return StatementKind::dml;
  }
  if (first == "pragma") {
    return StatementKind::session;
  }
  if (first == "select" || first == "values" || first == "explain") {
    return StatementKind::query;
  }
  return StatementKind::unknown;
}

[[nodiscard]] std::string pragma_name(const std::string_view sql) {
  const auto words = statement_words(sql, 4U);
  if (words.size() < 2U || words[0] != "pragma") {
    return {};
  }
  if (words[1] == "main" && words.size() >= 3U) {
    return words[2];
  }
  return words[1];
}

[[nodiscard]] bool is_create_table_as_select(const std::string_view sql) {
  const auto words = statement_words(sql, 5U);
  if (words.size() < 2U || words[0] != "create") {
    return false;
  }

  std::size_t table_position = 1U;
  if (words[table_position] == "temp" || words[table_position] == "temporary") {
    ++table_position;
  }
  if (table_position >= words.size() || words[table_position] != "table") {
    return false;
  }

  bool saw_open_parenthesis = false;
  std::size_t position = 0;
  while (position < sql.size()) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }
    const auto character = sql[position];
    if (character == '(') {
      saw_open_parenthesis = true;
      ++position;
      continue;
    }
    if (!saw_open_parenthesis && is_word_start(character)) {
      const auto begin = position++;
      while (position < sql.size() && is_word_continue(sql[position])) {
        ++position;
      }
      if (ascii_lower(sql.substr(begin, position - begin)) == "as") {
        return true;
      }
      continue;
    }
    if (character == '\'' || character == '"' || character == '`' || character == '[') {
      const auto closing = character == '[' ? ']' : character;
      ++position;
      while (position < sql.size()) {
        if (sql[position] == closing) {
          if (character != '[' && position + 1U < sql.size() && sql[position + 1U] == closing) {
            position += 2U;
            continue;
          }
          ++position;
          break;
        }
        ++position;
      }
      continue;
    }
    ++position;
  }
  return false;
}

[[nodiscard]] bool is_unsupported_create(const std::string_view sql) {
  const auto words = statement_words(sql, 4U);
  if (words.size() < 2U || words[0] != "create") {
    return false;
  }
  return words[1] == "temp" || words[1] == "temporary" || words[1] == "virtual";
}

enum class SqlTokenKind : std::uint8_t { word, quoted_identifier, string_literal, punctuation };

struct SqlToken {
  SqlTokenKind kind{SqlTokenKind::punctuation};
  std::string value;
  std::size_t begin{0};
};

[[nodiscard]] std::vector<SqlToken> sql_tokens(const std::string_view sql) {
  std::vector<SqlToken> tokens;
  std::size_t position = 0;
  while (position < sql.size()) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }

    const auto token_begin = position;
    const auto character = sql[position];
    if (is_word_start(character)) {
      const auto begin = position++;
      while (position < sql.size() && is_word_continue(sql[position])) {
        ++position;
      }
      tokens.push_back(
          SqlToken{SqlTokenKind::word, ascii_lower(sql.substr(begin, position - begin)), begin});
      continue;
    }
    if (character == '"' || character == '`' || character == '[') {
      const auto closing = character == '[' ? ']' : character;
      ++position;
      std::string identifier;
      bool closed = false;
      while (position < sql.size()) {
        if (sql[position] == closing) {
          if (character != '[' && position + 1U < sql.size() && sql[position + 1U] == closing) {
            identifier.push_back(closing);
            position += 2U;
            continue;
          }
          ++position;
          closed = true;
          break;
        }
        identifier.push_back(sql[position++]);
      }
      if (!closed) {
        throw_migration("unterminated quoted identifier in SQLite script");
      }
      tokens.push_back(
          SqlToken{SqlTokenKind::quoted_identifier, ascii_lower(identifier), token_begin});
      continue;
    }
    if (character == '\'') {
      ++position;
      bool closed = false;
      while (position < sql.size()) {
        if (sql[position] == '\'') {
          if (position + 1U < sql.size() && sql[position + 1U] == '\'') {
            position += 2U;
            continue;
          }
          ++position;
          closed = true;
          break;
        }
        ++position;
      }
      if (!closed) {
        throw_migration("unterminated string literal in SQLite script");
      }
      tokens.push_back(SqlToken{SqlTokenKind::string_literal, {}, token_begin});
      continue;
    }
    tokens.push_back(SqlToken{SqlTokenKind::punctuation, std::string{character}, token_begin});
    ++position;
  }
  return tokens;
}

[[nodiscard]] bool is_keyword(const SqlToken& token, const std::string_view keyword) {
  return token.kind == SqlTokenKind::word && token.value == keyword;
}

[[nodiscard]] bool is_identifier(const SqlToken& token) {
  return token.kind == SqlTokenKind::word || token.kind == SqlTokenKind::quoted_identifier;
}

[[nodiscard]] bool ddl_targets_unsupported_schema(const std::string_view sql) {
  const auto tokens = sql_tokens(sql);
  if (tokens.size() < 2U) {
    return false;
  }

  std::size_t position = 1U;
  if (is_keyword(tokens[0], "create")) {
    if (is_keyword(tokens[position], "temp") || is_keyword(tokens[position], "temporary") ||
        is_keyword(tokens[position], "virtual")) {
      return true;
    }
    if (is_keyword(tokens[position], "unique")) {
      ++position;
    }
  }

  if (position >= tokens.size() ||
      (!is_keyword(tokens[position], "table") && !is_keyword(tokens[position], "index") &&
       !is_keyword(tokens[position], "view") && !is_keyword(tokens[position], "trigger"))) {
    return false;
  }
  ++position;
  if (position + 2U < tokens.size() && is_keyword(tokens[position], "if") &&
      is_keyword(tokens[position + 1U], "not") && is_keyword(tokens[position + 2U], "exists")) {
    position += 3U;
  } else if (position + 1U < tokens.size() && is_keyword(tokens[position], "if") &&
             is_keyword(tokens[position + 1U], "exists")) {
    position += 2U;
  }

  return position + 2U < tokens.size() && is_identifier(tokens[position]) &&
         tokens[position + 1U].kind == SqlTokenKind::punctuation &&
         tokens[position + 1U].value == "." && tokens[position].value != "main";
}

void append_canonical_token(std::string& output, const char kind, const std::string_view value) {
  output.push_back(kind);
  output.append(std::to_string(value.size()));
  output.push_back(':');
  output.append(value);
  output.push_back(';');
}

[[nodiscard]] std::string canonicalize_sql(const std::string_view sql,
                                           const std::set<std::size_t>& declarations) {
  std::string result;
  std::size_t position = 0;
  while (position < sql.size()) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }

    const auto token_begin = position;
    const auto character = sql[position];
    if (is_word_start(character) || std::isdigit(static_cast<unsigned char>(character)) != 0) {
      const auto begin = position++;
      while (position < sql.size() && is_word_continue(sql[position])) {
        ++position;
      }
      append_canonical_token(result, 'a', ascii_lower(sql.substr(begin, position - begin)));
      continue;
    }

    if (character == '"' || character == '`' || character == '[') {
      const auto closing = character == '[' ? ']' : character;
      ++position;
      std::string identifier;
      bool closed = false;
      while (position < sql.size()) {
        if (sql[position] == closing) {
          if (character != '[' && position + 1U < sql.size() && sql[position + 1U] == closing) {
            identifier.push_back(closing);
            position += 2U;
            continue;
          }
          ++position;
          closed = true;
          break;
        }
        identifier.push_back(sql[position++]);
      }
      if (!closed) {
        throw_unsupported("unterminated quoted identifier in stored SQLite schema");
      }
      // Expression quotes may denote identifiers or SQLite's legacy string
      // syntax. Keep both the quote family and contents until the caller has
      // established that this token occupies a declaration-name position.
      if (declarations.contains(token_begin)) {
        append_canonical_token(result, 'a', ascii_lower(identifier));
      } else {
        append_canonical_token(result, character, identifier);
      }
      continue;
    }

    if (character == '\'') {
      ++position;
      std::string literal;
      bool closed = false;
      while (position < sql.size()) {
        if (sql[position] == '\'') {
          if (position + 1U < sql.size() && sql[position + 1U] == '\'') {
            literal.push_back('\'');
            position += 2U;
            continue;
          }
          ++position;
          closed = true;
          break;
        }
        literal.push_back(sql[position++]);
      }
      if (!closed) {
        throw_unsupported("unterminated string literal in stored SQLite schema");
      }
      append_canonical_token(result, declarations.contains(token_begin) ? 'a' : 's',
                             declarations.contains(token_begin) ? ascii_lower(literal) : literal);
      continue;
    }

    append_canonical_token(result, 'p', std::string_view{&sql[position], 1U});
    ++position;
  }
  return result;
}

[[nodiscard]] std::string canonicalize_schema_sql(const std::string_view sql) {
  const auto tokens = sql_tokens(sql);
  std::set<std::size_t> declarations;
  if (tokens.size() < 3U || !is_keyword(tokens[0], "create")) {
    return canonicalize_sql(sql);
  }
  std::size_t kind = 1U;
  if (is_keyword(tokens[kind], "unique")) {
    ++kind;
  }
  if (kind + 1U >= tokens.size() ||
      (!is_keyword(tokens[kind], "table") && !is_keyword(tokens[kind], "index") &&
       !is_keyword(tokens[kind], "view") && !is_keyword(tokens[kind], "trigger"))) {
    return canonicalize_sql(sql);
  }
  std::size_t name = kind + 1U;
  if (name + 2U < tokens.size() && is_keyword(tokens[name], "if") &&
      is_keyword(tokens[name + 1U], "not") && is_keyword(tokens[name + 2U], "exists")) {
    name += 3U;
  }
  if (name >= tokens.size()) {
    return canonicalize_sql(sql);
  }
  declarations.insert(tokens[name].begin);
  if (name + 2U < tokens.size() && tokens[name + 1U].value == ".") {
    name += 2U;
    declarations.insert(tokens[name].begin);
  }
  if (is_keyword(tokens[kind], "table") && name + 1U < tokens.size() &&
      tokens[name + 1U].value == "(") {
    int depth = 1;
    bool item_start = true;
    for (std::size_t index = name + 2U; index < tokens.size() && depth > 0; ++index) {
      const auto& token = tokens[index];
      if (item_start) {
        if (!is_keyword(token, "constraint") && !is_keyword(token, "primary") &&
            !is_keyword(token, "unique") && !is_keyword(token, "check") &&
            !is_keyword(token, "foreign")) {
          declarations.insert(token.begin);
        }
        item_start = false;
      }
      if (token.kind == SqlTokenKind::punctuation) {
        if (token.value == "(") {
          ++depth;
        } else if (token.value == ")") {
          --depth;
        } else if (token.value == "," && depth == 1) {
          item_start = true;
        }
      }
    }
  }
  return canonicalize_sql(sql, declarations);
}

[[nodiscard]] std::string_view trim_sql(const std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && is_space(value[begin])) {
    ++begin;
  }
  auto end = value.size();
  while (end > begin && is_space(value[end - 1U])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

[[nodiscard]] std::string quote_identifier(const std::string_view identifier) {
  std::string result{"\""};
  result.reserve(identifier.size() + 2U);
  for (const char character : identifier) {
    if (character == '"') {
      result.push_back('"');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

[[nodiscard]] std::string quote_literal(const std::string_view literal) {
  std::string result{"'"};
  result.reserve(literal.size() + 2U);
  for (const char character : literal) {
    if (character == '\'') {
      result.push_back('\'');
    }
    result.push_back(character);
  }
  result.push_back('\'');
  return result;
}

[[nodiscard]] std::string terminated_statement(const std::string_view sql) {
  const auto trimmed = trim_sql(sql);
  if (trimmed.empty()) {
    throw_unsupported("cannot render an empty SQLite schema statement");
  }
  std::string result{trimmed};
  if (!result.ends_with(';')) {
    result.push_back(';');
  }
  return result;
}

[[nodiscard]] std::size_t skip_quoted_schema_token(const std::string_view sql,
                                                   std::size_t position) {
  const auto opening = sql[position];
  const auto closing = opening == '[' ? ']' : opening;
  ++position;
  while (position < sql.size()) {
    if (sql[position] == closing) {
      if (opening != '[' && position + 1U < sql.size() && sql[position + 1U] == closing) {
        position += 2U;
        continue;
      }
      return position + 1U;
    }
    ++position;
  }
  throw_unsupported("unterminated quoted token in stored SQLite table definition");
}

[[nodiscard]] bool table_constraint_item(const std::string_view item) {
  auto position = skip_space_and_comments(item, 0);
  if (position >= item.size() || item[position] == '"' || item[position] == '`' ||
      item[position] == '[' || !is_word_start(item[position])) {
    return false;
  }
  const auto begin = position++;
  while (position < item.size() && is_word_continue(item[position])) {
    ++position;
  }
  const auto word = ascii_lower(item.substr(begin, position - begin));
  return word == "constraint" || word == "primary" || word == "unique" || word == "check" ||
         word == "foreign";
}

[[nodiscard]] TableDefinition parse_table_definition(const TableSnapshot& table) {
  const auto sql = std::string_view{table.create_sql};
  std::size_t open = std::string_view::npos;
  std::size_t position = 0;
  while (position < sql.size()) {
    const auto skipped = skip_space_and_comments(sql, position);
    if (skipped != position) {
      position = skipped;
      continue;
    }
    const auto character = sql[position];
    if (character == '\'' || character == '"' || character == '`' || character == '[') {
      position = skip_quoted_schema_token(sql, position);
      continue;
    }
    if (character == '(') {
      open = position;
      break;
    }
    ++position;
  }
  if (open == std::string_view::npos) {
    throw_unsupported("unsupported CREATE TABLE form for " + table.name);
  }

  std::vector<std::string> items;
  auto item_begin = open + 1U;
  position = item_begin;
  int depth = 0;
  std::size_t close = std::string_view::npos;
  while (position < sql.size()) {
    const auto skipped = skip_space_and_comments(sql, position);
    if (skipped != position) {
      position = skipped;
      continue;
    }
    const auto character = sql[position];
    if (character == '\'' || character == '"' || character == '`' || character == '[') {
      position = skip_quoted_schema_token(sql, position);
      continue;
    }
    if (character == '(') {
      ++depth;
      ++position;
      continue;
    }
    if (character == ')') {
      if (depth == 0) {
        const auto item = trim_sql(sql.substr(item_begin, position - item_begin));
        if (!item.empty()) {
          items.emplace_back(item);
        }
        close = position;
        break;
      }
      --depth;
      ++position;
      continue;
    }
    if (character == ',' && depth == 0) {
      const auto item = trim_sql(sql.substr(item_begin, position - item_begin));
      if (item.empty()) {
        throw_unsupported("empty item in CREATE TABLE definition for " + table.name);
      }
      items.emplace_back(item);
      item_begin = position + 1U;
    }
    ++position;
  }
  if (close == std::string_view::npos || depth != 0) {
    throw_unsupported("unbalanced CREATE TABLE definition for " + table.name);
  }

  TableDefinition result;
  result.open_parenthesis = open;
  result.close_parenthesis = close;
  for (auto& item : items) {
    if (table_constraint_item(item)) {
      result.constraints.push_back(std::move(item));
    } else {
      result.columns.push_back(std::move(item));
    }
  }
  if (result.columns.size() != table.columns.size()) {
    throw_unsupported("CREATE TABLE columns do not match table_xinfo for " + table.name);
  }
  result.trailing = std::string{trim_sql(sql.substr(close + 1U))};
  if (result.trailing.ends_with(';')) {
    result.trailing.pop_back();
    result.trailing = std::string{trim_sql(result.trailing)};
  }
  return result;
}

[[nodiscard]] bool contains_unquoted_word(const std::string_view sql,
                                          const std::string_view expected) {
  std::size_t position = 0;
  while (position < sql.size()) {
    const auto skipped = skip_space_and_comments(sql, position);
    if (skipped != position) {
      position = skipped;
      continue;
    }
    const auto character = sql[position];
    if (character == '\'' || character == '"' || character == '`' || character == '[') {
      position = skip_quoted_schema_token(sql, position);
      continue;
    }
    if (is_word_start(character)) {
      const auto begin = position++;
      while (position < sql.size() && is_word_continue(sql[position])) {
        ++position;
      }
      if (ascii_lower(sql.substr(begin, position - begin)) == expected) {
        return true;
      }
      continue;
    }
    ++position;
  }
  return false;
}

void validate_source_statements(const std::string_view sql,
                                const std::vector<StatementSpan>& statements) {
  for (const auto& statement : statements) {
    const auto text = sql.substr(statement.begin, statement.end - statement.begin);
    if (references_reserved_metadata(text)) {
      throw Error{ErrorCode::source,
                  "SQLite declarative sources may not reference reserved _dbdiff_ objects"};
    }
    if (statement.kind != StatementKind::ddl) {
      throw Error{ErrorCode::source,
                  "SQLite declarative sources may contain only persistent schema DDL"};
    }
    if (is_unsupported_create(text) || ddl_targets_unsupported_schema(text)) {
      throw Error{ErrorCode::source,
                  "temporary, attached, and virtual SQLite objects are not supported in sources"};
    }
    if (is_create_table_as_select(text)) {
      throw Error{ErrorCode::source, "CREATE TABLE AS SELECT is not declarative schema DDL"};
    }
  }
}

void validate_migration_statements(const std::string_view sql,
                                   const std::vector<StatementSpan>& statements) {
  bool in_transaction = false;
  for (const auto& statement : statements) {
    const auto text = sql.substr(statement.begin, statement.end - statement.begin);
    if (references_reserved_metadata(text)) {
      throw_migration("SQLite migrations may not reference reserved _dbdiff_ objects");
    }
    switch (statement.kind) {
    case StatementKind::begin:
      if (in_transaction) {
        throw_migration("nested BEGIN is not allowed in a SQLite migration");
      }
      in_transaction = true;
      break;
    case StatementKind::commit:
      if (!in_transaction) {
        throw_migration("COMMIT without a matching BEGIN in SQLite migration");
      }
      in_transaction = false;
      break;
    case StatementKind::rollback:
      throw_migration("full ROLLBACK is not allowed in a SQLite migration");
    case StatementKind::savepoint:
    case StatementKind::release_savepoint:
    case StatementKind::rollback_to_savepoint:
      if (!in_transaction) {
        throw_migration("SQLite savepoint control requires an explicit transaction");
      }
      break;
    case StatementKind::dml:
      if (!in_transaction) {
        throw_migration("standalone SQLite DML is not resumable; use BEGIN/COMMIT");
      }
      break;
    case StatementKind::ddl:
      if (is_unsupported_create(text) || ddl_targets_unsupported_schema(text)) {
        throw_migration("temporary, attached, and virtual SQLite objects are not supported");
      }
      if (is_create_table_as_select(text)) {
        throw_migration("CREATE TABLE AS SELECT is not safe migration DDL");
      }
      break;
    case StatementKind::session: {
      const auto name = pragma_name(text);
      if (name != "foreign_keys" && name != "foreign_key_check" && name != "defer_foreign_keys") {
        throw_migration("unsupported SQLite PRAGMA in migration: " + name);
      }
      if (name == "foreign_keys" && in_transaction) {
        throw_migration("PRAGMA foreign_keys has no effect inside a transaction");
      }
      break;
    }
    case StatementKind::query:
      throw_migration("standalone queries are not allowed in a SQLite migration");
    case StatementKind::unknown:
      throw_migration("unsupported statement in SQLite migration");
    }
  }
  if (in_transaction) {
    throw_migration("SQLite migration ends inside an explicit transaction");
  }
}

} // namespace dbdiff::sqlite::detail

namespace dbdiff::sqlite {
using namespace detail;
std::vector<StatementSpan> scan_statements(const std::string_view sql) {
  if (sql.find('\0') != std::string_view::npos) {
    throw_migration("SQLite script contains a NUL byte");
  }

  std::vector<StatementSpan> statements;
  std::size_t begin = 0;
  std::size_t candidate = 0;
  while ((candidate = sql.find(';', candidate)) != std::string_view::npos) {
    const auto end = candidate + 1U;
    const auto slice = sql.substr(begin, end - begin);
    const std::string terminated{slice};
    if (sqlite3_complete(terminated.c_str()) != 0) {
      if (has_sql(slice)) {
        statements.push_back(StatementSpan{begin, end, classify_statement(slice)});
      }
      begin = end;
    }
    candidate = end;
  }

  const auto tail = sql.substr(begin);
  if (has_sql(tail)) {
    std::string terminated{tail};
    terminated.push_back(';');
    if (sqlite3_complete(terminated.c_str()) == 0) {
      throw_migration("incomplete SQLite statement at end of script");
    }
    statements.push_back(StatementSpan{begin, sql.size(), classify_statement(tail)});
  }
  return statements;
}

} // namespace dbdiff::sqlite
