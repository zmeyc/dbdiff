#include "internal.hpp"

namespace dbdiff::postgresql {
namespace detail {

[[noreturn]] void script_error(const std::string& message) {
  throw Error{ErrorCode::migration, message};
}

[[nodiscard]] bool ascii_space(const char character) noexcept {
  return std::isspace(static_cast<unsigned char>(character)) != 0;
}

[[nodiscard]] bool word_start(const char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return std::isalpha(value) != 0 || character == '_';
}

[[nodiscard]] bool word_continue(const char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_' || character == '$';
}

[[nodiscard]] char ascii_lower(const char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<char>(character - 'A' + 'a');
  }
  return character;
}

[[nodiscard]] std::string ascii_lower(const std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    result.push_back(ascii_lower(character));
  }
  return result;
}

[[nodiscard]] std::size_t consume_line_comment(const std::string_view sql,
                                               std::size_t position) noexcept {
  position += 2U;
  while (position < sql.size() && sql[position] != '\n') {
    ++position;
  }
  return position;
}

[[nodiscard]] std::size_t consume_block_comment(const std::string_view sql, std::size_t position) {
  std::size_t depth = 1U;
  position += 2U;
  while (position < sql.size()) {
    if (position + 1U < sql.size() && sql[position] == '/' && sql[position + 1U] == '*') {
      ++depth;
      position += 2U;
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '*' && sql[position + 1U] == '/') {
      --depth;
      position += 2U;
      if (depth == 0U) {
        return position;
      }
      continue;
    }
    ++position;
  }
  script_error("unterminated block comment in PostgreSQL script");
}

[[nodiscard]] std::size_t skip_space_and_comments(const std::string_view sql,
                                                  std::size_t position) {
  while (position < sql.size()) {
    if (ascii_space(sql[position])) {
      ++position;
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '-' && sql[position + 1U] == '-') {
      position = consume_line_comment(sql, position);
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '/' && sql[position + 1U] == '*') {
      position = consume_block_comment(sql, position);
      continue;
    }
    break;
  }
  return position;
}

[[nodiscard]] bool has_sql(const std::string_view sql) {
  return skip_space_and_comments(sql, 0U) != sql.size();
}

[[nodiscard]] std::size_t consume_single_quote(const std::string_view sql, std::size_t position) {
  const bool escape_string = position > 0U &&
                             (sql[position - 1U] == 'e' || sql[position - 1U] == 'E') &&
                             (position == 1U || !word_continue(sql[position - 2U]));
  ++position;
  while (position < sql.size()) {
    if (escape_string && sql[position] == '\\') {
      position += std::min<std::size_t>(2U, sql.size() - position);
      continue;
    }
    if (sql[position] == '\'') {
      if (position + 1U < sql.size() && sql[position + 1U] == '\'') {
        position += 2U;
        continue;
      }
      return position + 1U;
    }
    ++position;
  }
  script_error("unterminated string literal in PostgreSQL script");
}

[[nodiscard]] std::size_t consume_double_quote(const std::string_view sql, std::size_t position) {
  ++position;
  while (position < sql.size()) {
    if (sql[position] == '"') {
      if (position + 1U < sql.size() && sql[position + 1U] == '"') {
        position += 2U;
        continue;
      }
      return position + 1U;
    }
    ++position;
  }
  script_error("unterminated quoted identifier in PostgreSQL script");
}

[[nodiscard]] std::optional<std::string_view>
dollar_quote_delimiter(const std::string_view sql, const std::size_t position) noexcept {
  if (sql[position] != '$' || (position != 0U && word_continue(sql[position - 1U]))) {
    return std::nullopt;
  }
  auto end = position + 1U;
  if (end < sql.size() && sql[end] == '$') {
    return sql.substr(position, 2U);
  }
  if (end >= sql.size() || !word_start(sql[end])) {
    return std::nullopt;
  }
  ++end;
  while (end < sql.size() &&
         (std::isalnum(static_cast<unsigned char>(sql[end])) != 0 || sql[end] == '_')) {
    ++end;
  }
  if (end >= sql.size() || sql[end] != '$') {
    return std::nullopt;
  }
  return sql.substr(position, end - position + 1U);
}

[[nodiscard]] std::size_t consume_dollar_quote(const std::string_view sql,
                                               const std::size_t position,
                                               const std::string_view delimiter) {
  const auto closing = sql.find(delimiter, position + delimiter.size());
  if (closing == std::string_view::npos) {
    script_error("unterminated dollar-quoted string in PostgreSQL script");
  }
  return closing + delimiter.size();
}

[[nodiscard]] std::vector<std::string> statement_words(const std::string_view sql,
                                                       const std::size_t maximum = 8U) {
  std::vector<std::string> words;
  std::size_t position = 0U;
  while (position < sql.size() && words.size() < maximum) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }
    if (word_start(sql[position])) {
      const auto begin = position++;
      while (position < sql.size() && word_continue(sql[position])) {
        ++position;
      }
      words.push_back(ascii_lower(sql.substr(begin, position - begin)));
      continue;
    }
    if (sql[position] == '\'') {
      position = consume_single_quote(sql, position);
      continue;
    }
    if (sql[position] == '"') {
      position = consume_double_quote(sql, position);
      continue;
    }
    if (sql[position] == '$') {
      if (const auto delimiter = dollar_quote_delimiter(sql, position); delimiter.has_value()) {
        position = consume_dollar_quote(sql, position, *delimiter);
        continue;
      }
    }
    ++position;
  }
  return words;
}

[[nodiscard]] std::vector<std::string> top_level_statement_words(const std::string_view sql,
                                                                 const std::size_t maximum = 32U) {
  std::vector<std::string> words;
  std::size_t position = 0U;
  std::size_t parenthesis_depth = 0U;
  while (position < sql.size() && words.size() < maximum) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }
    if (word_start(sql[position])) {
      const auto begin = position++;
      while (position < sql.size() && word_continue(sql[position])) {
        ++position;
      }
      if (parenthesis_depth == 0U) {
        words.push_back(ascii_lower(sql.substr(begin, position - begin)));
      }
      continue;
    }
    if (sql[position] == '\'') {
      position = consume_single_quote(sql, position);
      continue;
    }
    if (sql[position] == '"') {
      position = consume_double_quote(sql, position);
      continue;
    }
    if (sql[position] == '$') {
      if (const auto delimiter = dollar_quote_delimiter(sql, position); delimiter.has_value()) {
        position = consume_dollar_quote(sql, position, *delimiter);
        continue;
      }
    }
    if (sql[position] == '(') {
      ++parenthesis_depth;
    } else if (sql[position] == ')' && parenthesis_depth > 0U) {
      --parenthesis_depth;
    }
    ++position;
  }
  return words;
}

[[nodiscard]] StatementKind classify_statement(const std::string_view sql) {
  const auto words = statement_words(sql, 4U);
  if (words.empty()) {
    return StatementKind::unknown;
  }
  const auto& first = words.front();
  if (first == "begin" || (first == "start" && words.size() > 1U && words[1] == "transaction")) {
    return StatementKind::begin;
  }
  if (first == "commit" || first == "end") {
    return StatementKind::commit;
  }
  if (first == "rollback") {
    return std::ranges::find(words, "to") == words.end() ? StatementKind::rollback
                                                         : StatementKind::rollback_to_savepoint;
  }
  if (first == "savepoint") {
    return StatementKind::savepoint;
  }
  if (first == "release") {
    return StatementKind::release_savepoint;
  }
  if (first == "create" || first == "alter" || first == "drop" || first == "comment" ||
      first == "grant" || first == "revoke" || first == "security" || first == "label") {
    return StatementKind::ddl;
  }
  if (first == "insert" || first == "update" || first == "delete" || first == "merge" ||
      first == "copy" || first == "truncate" || first == "with" || first == "call" ||
      first == "do") {
    return StatementKind::dml;
  }
  if (first == "set" || first == "reset" || first == "discard" || first == "listen" ||
      first == "unlisten" || first == "notify" || first == "vacuum" || first == "analyze" ||
      first == "reindex" || first == "cluster" || first == "refresh" || first == "checkpoint") {
    return StatementKind::session;
  }
  if (first == "select" || first == "values" || first == "table" || first == "explain" ||
      first == "show") {
    return StatementKind::query;
  }
  return StatementKind::unknown;
}

[[nodiscard]] bool transaction_control(const StatementKind kind) noexcept {
  return kind == StatementKind::begin || kind == StatementKind::commit ||
         kind == StatementKind::rollback || kind == StatementKind::savepoint ||
         kind == StatementKind::release_savepoint || kind == StatementKind::rollback_to_savepoint;
}

[[nodiscard]] bool references_metadata_schema(const std::string_view sql) {
  std::size_t position = 0U;
  while (position < sql.size()) {
    if (sql[position] == '\'') {
      position = consume_single_quote(sql, position);
      continue;
    }
    if (sql[position] == '"') {
      std::string identifier;
      ++position;
      while (position < sql.size()) {
        if (sql[position] == '"') {
          if (position + 1U < sql.size() && sql[position + 1U] == '"') {
            identifier.push_back('"');
            position += 2U;
            continue;
          }
          ++position;
          break;
        }
        identifier.push_back(sql[position++]);
      }
      if (identifier == metadata_schema) {
        return true;
      }
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '-' && sql[position + 1U] == '-') {
      position = consume_line_comment(sql, position);
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '/' && sql[position + 1U] == '*') {
      position = consume_block_comment(sql, position);
      continue;
    }
    if (sql[position] == '$') {
      if (const auto delimiter = dollar_quote_delimiter(sql, position); delimiter.has_value()) {
        position = consume_dollar_quote(sql, position, *delimiter);
        continue;
      }
    }
    if (word_start(sql[position])) {
      const auto begin = position++;
      while (position < sql.size() && word_continue(sql[position])) {
        ++position;
      }
      if (ascii_lower(sql.substr(begin, position - begin)) == metadata_schema) {
        return true;
      }
      continue;
    }
    ++position;
  }
  return false;
}

[[nodiscard]] bool source_forbidden_ddl(const std::string_view sql) {
  const auto words = statement_words(sql, 8U);
  if (words.size() < 2U) {
    return true;
  }
  if (words[0] == "alter" && words[1] == "system") {
    return true;
  }
  if (words[0] == "grant" || words[0] == "revoke" || words[0] == "comment" ||
      words[0] == "security" || words[0] == "label" ||
      (words[0] == "alter" && words[1] == "default")) {
    return true;
  }
  if ((words[0] == "create" || words[0] == "alter" || words[0] == "drop") &&
      (words[1] == "database" || words[1] == "tablespace" || words[1] == "role" ||
       words[1] == "user" || words[1] == "group" || words[1] == "subscription" ||
       words[1] == "publication" || words[1] == "extension" || words[1] == "server" ||
       words[1] == "event" || words[1] == "language" || words[1] == "access" ||
       words[1] == "cast" || words[1] == "transform" || words[1] == "foreign")) {
    return true;
  }
  return words[0] == "create" && (std::ranges::find(words, "temp") != words.end() ||
                                  std::ranges::find(words, "temporary") != words.end());
}

[[nodiscard]] bool creates_table_from_query(const std::string_view sql) {
  const auto words = top_level_statement_words(sql);
  if (words.size() < 3U || words[0] != "create") {
    return false;
  }

  std::size_t table_position = 1U;
  if (words[table_position] == "unlogged" || words[table_position] == "temporary" ||
      words[table_position] == "temp") {
    ++table_position;
  } else if ((words[table_position] == "global" || words[table_position] == "local") &&
             table_position + 1U < words.size() &&
             (words[table_position + 1U] == "temporary" || words[table_position + 1U] == "temp")) {
    table_position += 2U;
  }
  if (table_position >= words.size() || words[table_position] != "table") {
    return false;
  }
  return std::ranges::find(words.begin() + static_cast<std::ptrdiff_t>(table_position + 1U),
                           words.end(), "as") != words.end();
}

[[nodiscard]] bool server_wide_statement(const std::string_view sql) {
  const auto words = top_level_statement_words(sql);
  if (words.empty()) {
    return false;
  }
  if (words[0] == "checkpoint" ||
      (words[0] == "alter" && words.size() > 1U && words[1] == "system")) {
    return true;
  }

  if ((words[0] == "create" || words[0] == "alter" || words[0] == "drop") && words.size() > 1U) {
    const auto& object = words[1];
    if (object == "database" || object == "tablespace" || object == "role" || object == "group" ||
        object == "subscription") {
      return true;
    }
    if (object == "user" && (words.size() < 3U || words[2] != "mapping")) {
      return true;
    }
  }

  if ((words[0] == "comment" || words[0] == "security") && words.size() > 2U && words[1] == "on" &&
      (words[2] == "database" || words[2] == "tablespace" || words[2] == "role")) {
    return true;
  }

  if (words[0] == "grant" || words[0] == "revoke") {
    const auto on = std::ranges::find(words, "on");
    if (on == words.end()) {
      return true;
    }
    const auto target = std::next(on);
    return target != words.end() &&
           (*target == "database" || *target == "tablespace" || *target == "parameter");
  }
  return false;
}

void validate_source_statements(const std::string_view sql,
                                const std::vector<StatementSpan>& statements) {
  for (const auto& statement : statements) {
    if (transaction_control(statement.kind)) {
      throw Error{ErrorCode::source,
                  "PostgreSQL declarative sources must not contain transaction control"};
    }
    if (statement.kind != StatementKind::ddl) {
      throw Error{ErrorCode::source,
                  "PostgreSQL declarative sources may contain only persistent schema DDL"};
    }
    const auto text = sql.substr(statement.begin, statement.end - statement.begin);
    if (references_metadata_schema(text)) {
      throw Error{ErrorCode::source,
                  "PostgreSQL declarative sources must not access the dbdiff metadata schema"};
    }
    if (source_forbidden_ddl(text)) {
      throw Error{ErrorCode::source,
                  "PostgreSQL declarative sources must not contain temporary or server-wide DDL"};
    }
    if (creates_table_from_query(text)) {
      throw Error{ErrorCode::source,
                  "PostgreSQL declarative sources must not create tables from query results"};
    }
  }
}

[[nodiscard]] bool changes_string_lexing(const std::string_view sql) {
  const auto words = statement_words(sql, 8U);
  if (words.empty()) {
    return false;
  }
  if (words[0] == "discard" && words.size() > 1U && words[1] == "all") {
    return true;
  }
  if (words[0] != "set" && words[0] != "reset") {
    return false;
  }
  return std::ranges::find(words, "standard_conforming_strings") != words.end();
}

void validate_migration_statements(const std::string_view sql,
                                   const std::vector<StatementSpan>& statements) {
  for (const auto& statement : statements) {
    if (statement.kind == StatementKind::query) {
      throw Error{ErrorCode::migration,
                  "standalone PostgreSQL queries are not allowed in migrations"};
    }
    if (statement.kind == StatementKind::unknown) {
      throw Error{ErrorCode::migration, "unsupported statement in PostgreSQL migration"};
    }
    const auto text = sql.substr(statement.begin, statement.end - statement.begin);
    if (references_metadata_schema(text)) {
      throw Error{ErrorCode::migration,
                  "PostgreSQL migrations must not access the dbdiff metadata schema"};
    }
    if (server_wide_statement(text)) {
      throw Error{ErrorCode::migration,
                  "PostgreSQL migrations must not contain server-wide statements"};
    }
    const auto words = statement_words(text, 8U);
    if (statement.kind == StatementKind::commit &&
        (std::ranges::find(words, "chain") != words.end() ||
         std::ranges::find(words, "prepared") != words.end())) {
      throw Error{ErrorCode::migration,
                  "PostgreSQL migrations require a plain COMMIT transaction boundary"};
    }
    if (changes_string_lexing(text)) {
      throw Error{ErrorCode::migration,
                  "PostgreSQL migrations must not change standard_conforming_strings"};
    }
  }
}

void rollback_server_transaction_noexcept(pqxx::nontransaction& transaction) noexcept {
  try {
    execute_no_rows(transaction, "ROLLBACK;");
  } catch (const std::exception& error) {
    static_cast<void>(error);
  }
}

[[nodiscard]] std::string_view unit_sql(const ParsedScript& parsed, const ExecutionUnit& unit) {
  return std::string_view{parsed.sql}.substr(unit.begin, unit.end - unit.begin);
}

void execute_plain_unit(pqxx::nontransaction& transaction, const ParsedScript& parsed,
                        const ExecutionUnit& unit) {
  execute_no_rows(transaction, unit_sql(parsed, unit));
}

void execute_transaction_unit(pqxx::nontransaction& transaction, const ParsedScript& parsed,
                              const ExecutionUnit& unit,
                              const std::function<void()>& before_commit) {
  bool inside_server_transaction = false;
  try {
    for (const auto& statement : unit.statements) {
      if (statement.kind == StatementKind::commit) {
        before_commit();
      }
      const auto text =
          std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin);
      execute_no_rows(transaction, text);
      if (statement.kind == StatementKind::begin) {
        inside_server_transaction = true;
      } else if (statement.kind == StatementKind::commit) {
        inside_server_transaction = false;
      }
    }
  } catch (...) {
    if (inside_server_transaction) {
      rollback_server_transaction_noexcept(transaction);
    }
    throw;
  }
}

[[nodiscard]] std::vector<std::string> unit_words(const ParsedScript& parsed,
                                                  const ExecutionUnit& unit) {
  if (unit.statements.empty()) {
    return {};
  }
  const auto& statement = unit.statements.front();
  return statement_words(
      std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin));
}

[[nodiscard]] bool replayable_session_unit(const ParsedScript& parsed, const ExecutionUnit& unit) {
  if (unit.explicit_transaction || unit.statements.size() != 1U ||
      unit.statements.front().kind != StatementKind::session) {
    return false;
  }
  const auto words = unit_words(parsed, unit);
  return !words.empty() && (words[0] == "set" || words[0] == "reset" || words[0] == "discard" ||
                            words[0] == "listen" || words[0] == "unlisten");
}

void validate_resumable_session_units(const ParsedScript& parsed) {
  for (const auto& unit : parsed.units) {
    if (!unit.explicit_transaction) {
      continue;
    }
    for (const auto& statement : unit.statements) {
      if (statement.kind != StatementKind::session) {
        continue;
      }
      const auto words = statement_words(
          std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin));
      if (words.empty()) {
        continue;
      }
      const bool transaction_local_set =
          words[0] == "set" && words.size() > 1U &&
          (words[1] == "local" || words[1] == "transaction" || words[1] == "constraints");
      if (!transaction_local_set &&
          (words[0] == "set" || words[0] == "reset" || words[0] == "discard" ||
           words[0] == "listen" || words[0] == "unlisten")) {
        throw Error{ErrorCode::migration,
                    "persistent PostgreSQL session state inside a transaction unit is not "
                    "resumable; use a standalone session statement"};
      }
    }
  }
}

void execute_unit_prefix(pqxx::nontransaction& transaction, const ParsedScript& parsed,
                         const std::size_t completed_unit_count) {
  for (std::size_t index = 0U; index < completed_unit_count; ++index) {
    const auto& unit = parsed.units[index];
    if (unit.explicit_transaction) {
      execute_transaction_unit(transaction, parsed, unit, [] {});
    } else {
      execute_plain_unit(transaction, parsed, unit);
    }
  }
}

std::vector<std::string> index_key_definitions(const std::string_view definition,
                                               const std::size_t key_count) {
  std::vector<std::string> keys;
  std::size_t depth = 0U;
  std::size_t key_begin = 0U;
  const auto append_key = [&](const std::size_t end) {
    auto begin = key_begin;
    auto trimmed_end = end;
    while (begin < trimmed_end && ascii_space(definition[begin])) {
      ++begin;
    }
    while (trimmed_end > begin && ascii_space(definition[trimmed_end - 1U])) {
      --trimmed_end;
    }
    if (begin == trimmed_end) {
      throw Error{ErrorCode::database, "PostgreSQL returned an empty index key definition"};
    }
    keys.emplace_back(definition.substr(begin, trimmed_end - begin));
  };

  // PostgreSQL's full deparser retains COLLATE, operator classes and their options.
  // The column-only form omits those decorations. Preserve the server's key SQL
  // verbatim, splitting only the outer key list rather than interpreting expressions.
  for (std::size_t position = 0U; position < definition.size();) {
    if (definition[position] == '\'') {
      position = consume_single_quote(definition, position);
      continue;
    }
    if (definition[position] == '"') {
      position = consume_double_quote(definition, position);
      continue;
    }
    if (position + 1U < definition.size() && definition[position] == '-' &&
        definition[position + 1U] == '-') {
      position = consume_line_comment(definition, position);
      continue;
    }
    if (position + 1U < definition.size() && definition[position] == '/' &&
        definition[position + 1U] == '*') {
      position = consume_block_comment(definition, position);
      continue;
    }
    if (definition[position] == '$') {
      if (const auto delimiter = dollar_quote_delimiter(definition, position)) {
        position = consume_dollar_quote(definition, position, *delimiter);
        continue;
      }
    }
    if (definition[position] == '(') {
      if (depth++ == 0U) {
        key_begin = position + 1U;
      }
    } else if (definition[position] == ')' && depth != 0U) {
      if (--depth == 0U) {
        append_key(position);
        if (keys.size() != key_count) {
          throw Error{ErrorCode::database,
                      "PostgreSQL index key definitions do not match the catalog key count"};
        }
        return keys;
      }
    } else if (definition[position] == ',' && depth == 1U) {
      append_key(position);
      key_begin = position + 1U;
    }
    ++position;
  }
  throw Error{ErrorCode::database, "PostgreSQL returned an incomplete index key list"};
}

} // namespace detail

using namespace detail;

std::vector<StatementSpan> scan_statements(const std::string_view sql) {
  if (contains_nul(sql)) {
    script_error("PostgreSQL script contains a NUL byte");
  }

  std::vector<StatementSpan> statements;
  std::size_t begin = 0U;
  std::size_t position = 0U;
  while (position < sql.size()) {
    if (sql[position] == '\'') {
      position = consume_single_quote(sql, position);
      continue;
    }
    if (sql[position] == '"') {
      position = consume_double_quote(sql, position);
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '-' && sql[position + 1U] == '-') {
      position = consume_line_comment(sql, position);
      continue;
    }
    if (position + 1U < sql.size() && sql[position] == '/' && sql[position + 1U] == '*') {
      position = consume_block_comment(sql, position);
      continue;
    }
    if (sql[position] == '$') {
      if (const auto delimiter = dollar_quote_delimiter(sql, position); delimiter.has_value()) {
        position = consume_dollar_quote(sql, position, *delimiter);
        continue;
      }
    }
    if (sql[position] == ';') {
      const auto end = position + 1U;
      const auto text = sql.substr(begin, end - begin);
      if (has_sql(text)) {
        statements.push_back(StatementSpan{begin, end, classify_statement(text)});
      }
      begin = end;
      position = end;
      continue;
    }
    ++position;
  }

  const auto tail = sql.substr(begin);
  if (has_sql(tail)) {
    statements.push_back(StatementSpan{begin, sql.size(), classify_statement(tail)});
  }
  return statements;
}

void validate_source(const std::string_view sql) {
  const auto statements = scan_statements(sql);
  validate_source_statements(sql, statements);
}

ParsedScript parse_migration(std::string sql) {
  auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  return build_execution_units(std::move(sql), std::move(statements));
}

} // namespace dbdiff::postgresql
