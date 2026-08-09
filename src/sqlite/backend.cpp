#include "dbdiff/sqlite.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dbdiff::sqlite {
namespace {

constexpr int minimum_sqlite_version = 3'045'000;

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

[[nodiscard]] std::string sqlite_error_message(sqlite3* database, const int result,
                                               const std::string_view action) {
  std::ostringstream message;
  message << action << " failed (SQLite " << result << ')';
  if (database != nullptr) {
    message << ": " << sqlite3_errmsg(database);
    const auto offset = sqlite3_error_offset(database);
    if (offset >= 0) {
      message << " at SQL byte " << offset;
    }
  }
  return message.str();
}

[[noreturn]] void throw_sqlite(sqlite3* database, const int result, const std::string_view action) {
  throw Error{ErrorCode::database, sqlite_error_message(database, result, action)};
}

[[noreturn]] void throw_migration(const std::string& message) {
  throw Error{ErrorCode::migration, message};
}

[[noreturn]] void throw_unsupported(const std::string& message) {
  throw Error{ErrorCode::unsupported, message};
}

class Statement final {
public:
  Statement(sqlite3* database, const std::string_view sql) : database_{database} {
    if (sql.size() > static_cast<std::size_t>(INT_MAX)) {
      throw Error{ErrorCode::database, "SQLite statement exceeds the supported size"};
    }

    const char* tail = nullptr;
    const auto result = sqlite3_prepare_v3(database_, sql.data(), static_cast<int>(sql.size()), 0,
                                           &statement_, &tail);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "preparing statement");
    }
    consumed_ = static_cast<std::size_t>(tail - sql.data());
  }

  ~Statement() {
    if (statement_ != nullptr) {
      static_cast<void>(sqlite3_finalize(statement_));
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  [[nodiscard]] bool valid() const noexcept { return statement_ != nullptr; }
  [[nodiscard]] std::size_t consumed() const noexcept { return consumed_; }

  void bind_text(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
      throw Error{ErrorCode::database, "SQLite bound value exceeds the supported size"};
    }
    const auto result = sqlite3_bind_text(statement_, index, value.data(),
                                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "binding statement value");
    }
  }

  void bind_blob(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
      throw Error{ErrorCode::database, "SQLite bound value exceeds the supported size"};
    }
    const auto result = sqlite3_bind_blob(statement_, index, value.data(),
                                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "binding statement value");
    }
  }

  void bind_integer(const int index, const sqlite3_int64 value) {
    const auto result = sqlite3_bind_int64(statement_, index, value);
    if (result != SQLITE_OK) {
      throw_sqlite(database_, result, "binding statement value");
    }
  }

  [[nodiscard]] int step() {
    const auto result = sqlite3_step(statement_);
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
      throw_sqlite(database_, result, "executing statement");
    }
    return result;
  }

  [[nodiscard]] int integer(const int column) const {
    return sqlite3_column_int(statement_, column);
  }

  [[nodiscard]] sqlite3_int64 integer64(const int column) const {
    return sqlite3_column_int64(statement_, column);
  }

  [[nodiscard]] std::optional<std::string> optional_text(const int column) const {
    if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
      return std::nullopt;
    }
    const auto* value = sqlite3_column_text(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    if (value == nullptr || size < 0) {
      throw Error{ErrorCode::database, "SQLite returned an invalid text column"};
    }
    return std::string{reinterpret_cast<const char*>(value), static_cast<std::size_t>(size)};
  }

  [[nodiscard]] std::string text(const int column) const {
    auto value = optional_text(column);
    if (!value.has_value()) {
      throw Error{ErrorCode::database, "SQLite returned NULL for a required text column"};
    }
    return std::move(*value);
  }

  [[nodiscard]] std::string blob(const int column) const {
    if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
      throw Error{ErrorCode::database, "SQLite returned NULL for a required blob column"};
    }
    const auto* value = sqlite3_column_blob(statement_, column);
    const auto size = sqlite3_column_bytes(statement_, column);
    if ((value == nullptr && size != 0) || size < 0) {
      throw Error{ErrorCode::database, "SQLite returned an invalid blob column"};
    }
    if (size == 0) {
      return {};
    }
    return std::string{static_cast<const char*>(value), static_cast<std::size_t>(size)};
  }

private:
  sqlite3* database_{nullptr};
  sqlite3_stmt* statement_{nullptr};
  std::size_t consumed_{0};
};

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
};

[[nodiscard]] std::vector<SqlToken> sql_tokens(const std::string_view sql) {
  std::vector<SqlToken> tokens;
  std::size_t position = 0;
  while (position < sql.size()) {
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
      tokens.push_back(
          SqlToken{SqlTokenKind::word, ascii_lower(sql.substr(begin, position - begin))});
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
      tokens.push_back(SqlToken{SqlTokenKind::quoted_identifier, ascii_lower(identifier)});
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
      tokens.push_back(SqlToken{SqlTokenKind::string_literal, {}});
      continue;
    }
    tokens.push_back(SqlToken{SqlTokenKind::punctuation, std::string{character}});
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

[[nodiscard]] std::string canonicalize_sql(const std::string_view sql) {
  std::string result;
  std::size_t position = 0;
  while (position < sql.size()) {
    position = skip_space_and_comments(sql, position);
    if (position >= sql.size()) {
      break;
    }

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
      append_canonical_token(result, 'a', ascii_lower(identifier));
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
      append_canonical_token(result, 's', literal);
      continue;
    }

    append_canonical_token(result, 'p', std::string_view{&sql[position], 1U});
    ++position;
  }
  return result;
}

void require_consumed_statement(const std::string_view sql, const Statement& statement) {
  if (statement.consumed() > sql.size()) {
    throw Error{ErrorCode::database, "SQLite reported an invalid statement boundary"};
  }
  if (has_sql(sql.substr(statement.consumed()))) {
    throw Error{ErrorCode::database, "SQLite statement span contains more than one statement"};
  }
}

void execute_one(sqlite3* database, const std::string_view sql,
                 const bool reject_result_rows = false) {
  Statement statement{database, sql};
  if (!statement.valid()) {
    throw Error{ErrorCode::database, "SQLite did not compile an executable statement"};
  }
  require_consumed_statement(sql, statement);

  while (true) {
    const auto result = statement.step();
    if (result == SQLITE_DONE) {
      return;
    }
    if (reject_result_rows) {
      const auto table = statement.optional_text(0).value_or("<unknown>");
      const auto row = statement.optional_text(1).value_or("<unknown>");
      std::string message{"SQLite foreign-key violation in table "};
      message.append(table);
      message.append(" at row ");
      message.append(row);
      throw Error{ErrorCode::database, message};
    }
  }
}

int deny_unsafe_schema_action(void*, const int action, const char*, const char* second_argument,
                              const char*, const char*) noexcept {
  switch (action) {
  case SQLITE_ATTACH:
  case SQLITE_DETACH:
  case SQLITE_CREATE_VTABLE:
  case SQLITE_DROP_VTABLE:
  case SQLITE_CREATE_TEMP_INDEX:
  case SQLITE_CREATE_TEMP_TABLE:
  case SQLITE_CREATE_TEMP_TRIGGER:
  case SQLITE_CREATE_TEMP_VIEW:
  case SQLITE_DROP_TEMP_INDEX:
  case SQLITE_DROP_TEMP_TABLE:
  case SQLITE_DROP_TEMP_TRIGGER:
  case SQLITE_DROP_TEMP_VIEW:
    return SQLITE_DENY;
  case SQLITE_FUNCTION:
    return second_argument != nullptr && sqlite3_stricmp(second_argument, "load_extension") == 0
               ? SQLITE_DENY
               : SQLITE_OK;
  default:
    return SQLITE_OK;
  }
}

[[nodiscard]] int query_integer(sqlite3* database, const std::string_view sql) {
  Statement statement{database, sql};
  if (!statement.valid()) {
    throw Error{ErrorCode::database, "SQLite did not compile an integer query"};
  }
  require_consumed_statement(sql, statement);
  if (statement.step() != SQLITE_ROW) {
    throw Error{ErrorCode::database, "SQLite integer query returned no row"};
  }
  const auto value = statement.integer(0);
  if (statement.step() != SQLITE_DONE) {
    throw Error{ErrorCode::database, "SQLite integer query returned more than one row"};
  }
  return value;
}

void configure_database(sqlite3* database) {
  if (sqlite3_libversion_number() < minimum_sqlite_version) {
    throw Error{ErrorCode::unsupported,
                "SQLite 3.45.0 or newer is required; loaded " + std::string{sqlite3_libversion()}};
  }
  if (sqlite3_compileoption_used("OMIT_FOREIGN_KEY") != 0 ||
      sqlite3_compileoption_used("OMIT_TRIGGER") != 0) {
    throw Error{ErrorCode::unsupported,
                "the loaded SQLite library omits foreign-key or trigger support"};
  }

  auto result = sqlite3_extended_result_codes(database, 1);
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "enabling extended result codes");
  }
  result = sqlite3_busy_timeout(database, 5'000);
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "setting busy timeout");
  }

  const std::array settings{
      std::pair{SQLITE_DBCONFIG_DEFENSIVE, 1},
      std::pair{SQLITE_DBCONFIG_DQS_DDL, 0},
      std::pair{SQLITE_DBCONFIG_DQS_DML, 0},
      std::pair{SQLITE_DBCONFIG_LEGACY_ALTER_TABLE, 0},
      std::pair{SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0},
  };
  for (const auto& [setting, value] : settings) {
    int previous = 0;
    result = sqlite3_db_config(database, setting, value, &previous);
    if (result != SQLITE_OK) {
      throw_sqlite(database, result, "configuring SQLite connection");
    }
  }

  static_cast<void>(sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, 0));
  if (sqlite3_limit(database, SQLITE_LIMIT_ATTACHED, -1) != 0) {
    throw Error{ErrorCode::unsupported, "SQLite attached databases could not be disabled"};
  }
  result = sqlite3_set_authorizer(database, deny_unsafe_schema_action, nullptr);
  if (result != SQLITE_OK) {
    throw_sqlite(database, result, "installing SQLite schema safety policy");
  }

  execute_one(database, "PRAGMA foreign_keys=ON;");
  if (query_integer(database, "PRAGMA foreign_keys;") != 1) {
    throw Error{ErrorCode::unsupported, "SQLite foreign-key enforcement could not be enabled"};
  }
}

[[nodiscard]] int open_flags(const OpenMode mode) {
  constexpr auto common = SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_PRIVATECACHE;
  switch (mode) {
  case OpenMode::read_only:
    return SQLITE_OPEN_READONLY | common;
  case OpenMode::read_write:
    return SQLITE_OPEN_READWRITE | common;
  case OpenMode::read_write_create:
    return SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | common;
  }
  return SQLITE_OPEN_READONLY | common;
}

void validate_database_path(const std::filesystem::path& path) {
  const auto locator = path.string();
  if (locator.empty()) {
    throw Error{ErrorCode::database, "SQLite target path must not be empty"};
  }
  if (locator == ":memory:" || locator.starts_with("file:") ||
      locator.find_first_of("?#") != std::string::npos || locator.find('\0') != std::string::npos) {
    throw Error{ErrorCode::database, "SQLite target must be a plain persistent filesystem path"};
  }

  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    throw Error{ErrorCode::database, "cannot inspect SQLite target path: " + error.message()};
  }
  if (!error && std::filesystem::is_symlink(status)) {
    throw Error{ErrorCode::database, "SQLite target must not be a symbolic link"};
  }
  if (!error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
    throw Error{ErrorCode::database, "SQLite target must be a regular file"};
  }
}

[[nodiscard]] std::string optional_key(const std::optional<std::string>& value) {
  return value.has_value() ? *value : std::string{};
}

template <typename Item, typename Projection>
void stable_sort(std::vector<Item>& values, Projection projection) {
  std::ranges::sort(values, [&projection](const Item& left, const Item& right) {
    const auto left_value = projection(left);
    const auto right_value = projection(right);
    const auto left_key = ascii_lower(left_value);
    const auto right_key = ascii_lower(right_value);
    return std::tie(left_key, left_value) < std::tie(right_key, right_value);
  });
}

[[nodiscard]] std::map<std::pair<std::string, std::string>, std::optional<std::string>>
schema_sql(sqlite3* database) {
  std::map<std::pair<std::string, std::string>, std::optional<std::string>> result;
  Statement statement{database, "SELECT type,name,sql FROM main.sqlite_schema "
                                "WHERE type IN ('table','index','view','trigger') "
                                "ORDER BY type COLLATE BINARY,name COLLATE BINARY;"};
  while (statement.step() == SQLITE_ROW) {
    auto type = statement.text(0);
    auto name = statement.text(1);
    result.emplace(std::pair{std::move(type), std::move(name)}, statement.optional_text(2));
  }
  return result;
}

[[nodiscard]] std::vector<ColumnSnapshot> inspect_columns(sqlite3* database,
                                                          const std::string_view table) {
  Statement statement{database, "SELECT cid,name,type,\"notnull\",dflt_value,pk,hidden "
                                "FROM pragma_table_xinfo(?1,'main') ORDER BY cid;"};
  statement.bind_text(1, table);
  std::vector<ColumnSnapshot> columns;
  while (statement.step() == SQLITE_ROW) {
    const auto hidden = statement.integer(6);
    GeneratedColumnKind generated{};
    switch (hidden) {
    case 0:
      generated = GeneratedColumnKind::ordinary;
      break;
    case 2:
      generated = GeneratedColumnKind::virtual_generated;
      break;
    case 3:
      generated = GeneratedColumnKind::stored_generated;
      break;
    default:
      throw_unsupported("unsupported hidden SQLite column in table " + std::string{table});
    }
    columns.push_back(ColumnSnapshot{statement.integer(0), statement.text(1), statement.text(2),
                                     statement.integer(3) != 0, statement.optional_text(4),
                                     statement.integer(5), generated});
  }
  return columns;
}

[[nodiscard]] std::vector<ForeignKeySnapshot> inspect_foreign_keys(sqlite3* database,
                                                                   const std::string_view table) {
  Statement statement{database, "SELECT id,seq,\"table\",\"from\",\"to\",on_update,on_delete,match "
                                "FROM pragma_foreign_key_list(?1,'main') ORDER BY id,seq;"};
  statement.bind_text(1, table);

  std::vector<ForeignKeySnapshot> foreign_keys;
  while (statement.step() == SQLITE_ROW) {
    const auto id = statement.integer(0);
    if (foreign_keys.empty() || foreign_keys.back().id != id) {
      foreign_keys.push_back(ForeignKeySnapshot{
          id, statement.text(2), statement.text(5), statement.text(6), statement.text(7), {}});
    }
    foreign_keys.back().columns.push_back(ForeignKeyColumnSnapshot{
        statement.integer(1), statement.text(3), statement.optional_text(4)});
  }
  return foreign_keys;
}

[[nodiscard]] std::vector<IndexColumnSnapshot> inspect_index_columns(sqlite3* database,
                                                                     const std::string_view index) {
  Statement statement{database, "SELECT seqno,cid,name,\"desc\",coll,\"key\" "
                                "FROM pragma_index_xinfo(?1,'main') ORDER BY seqno;"};
  statement.bind_text(1, index);
  std::vector<IndexColumnSnapshot> columns;
  while (statement.step() == SQLITE_ROW) {
    columns.push_back(IndexColumnSnapshot{statement.integer(0), statement.integer(1),
                                          statement.optional_text(2), statement.integer(3) != 0,
                                          statement.optional_text(4), statement.integer(5) != 0});
  }
  return columns;
}

[[nodiscard]] std::vector<IndexSnapshot> inspect_indexes(
    sqlite3* database, const std::vector<TableSnapshot>& tables,
    const std::map<std::pair<std::string, std::string>, std::optional<std::string>>& definitions) {
  std::vector<IndexSnapshot> indexes;
  for (const auto& table : tables) {
    Statement statement{database, "SELECT name,\"unique\",origin,partial "
                                  "FROM pragma_index_list(?1,'main') ORDER BY seq;"};
    statement.bind_text(1, table.name);
    while (statement.step() == SQLITE_ROW) {
      auto name = statement.text(0);
      std::optional<std::string> create_sql;
      const auto definition = definitions.find({"index", name});
      if (definition != definitions.end()) {
        create_sql = definition->second;
      }
      indexes.push_back(IndexSnapshot{std::move(name),
                                      table.name,
                                      statement.integer(1) != 0,
                                      statement.text(2),
                                      statement.integer(3) != 0,
                                      std::move(create_sql),
                                      {}});
      indexes.back().columns = inspect_index_columns(database, indexes.back().name);
    }
  }
  std::ranges::sort(indexes, [](const IndexSnapshot& left, const IndexSnapshot& right) {
    return std::tuple{ascii_lower(left.table), ascii_lower(left.name), left.table, left.name} <
           std::tuple{ascii_lower(right.table), ascii_lower(right.name), right.table, right.name};
  });
  return indexes;
}

void hash_text(Sha256& hash, const std::string_view name, const std::string_view value) {
  hash.add_length_prefixed(name);
  hash.add_length_prefixed(value);
}

void hash_integer(Sha256& hash, const std::string_view name, const int value) {
  hash_text(hash, name, std::to_string(value));
}

void hash_boolean(Sha256& hash, const std::string_view name, const bool value) {
  hash_text(hash, name, value ? "1" : "0");
}

void hash_optional_sql(Sha256& hash, const std::string_view name,
                       const std::optional<std::string>& value) {
  hash_boolean(hash, "present", value.has_value());
  if (value.has_value()) {
    hash_text(hash, name, canonicalize_sql(*value));
  }
}

[[nodiscard]] std::string semantic_hash(const SchemaSnapshot& schema) {
  Sha256 hash;
  hash.add_length_prefixed("dbdiff.sqlite.schema.v1");
  for (const auto& table : schema.tables) {
    hash_text(hash, "object", "table");
    hash_text(hash, "name", table.name);
    hash_text(hash, "sql", canonicalize_sql(table.create_sql));
    hash_boolean(hash, "without_rowid", table.without_rowid);
    hash_boolean(hash, "strict", table.strict);
    for (const auto& column : table.columns) {
      hash_text(hash, "child", "column");
      hash_integer(hash, "rank", column.rank);
      hash_text(hash, "name", column.name);
      hash_text(hash, "type", canonicalize_sql(column.declared_type));
      hash_boolean(hash, "not_null", column.not_null);
      hash_optional_sql(hash, "default", column.default_sql);
      hash_integer(hash, "pk", column.primary_key_ordinal);
      hash_integer(hash, "generated", static_cast<int>(column.generated));
    }
    for (const auto& foreign_key : table.foreign_keys) {
      hash_text(hash, "child", "foreign_key");
      hash_integer(hash, "id", foreign_key.id);
      hash_text(hash, "parent", foreign_key.parent_table);
      hash_text(hash, "on_update", foreign_key.on_update);
      hash_text(hash, "on_delete", foreign_key.on_delete);
      hash_text(hash, "match", foreign_key.match);
      for (const auto& column : foreign_key.columns) {
        hash_integer(hash, "sequence", column.sequence);
        hash_text(hash, "from", column.from_column);
        hash_boolean(hash, "to_present", column.to_column.has_value());
        hash_text(hash, "to", optional_key(column.to_column));
      }
    }
  }
  for (const auto& index : schema.indexes) {
    hash_text(hash, "object", "index");
    hash_text(hash, "name", index.name);
    hash_text(hash, "table", index.table);
    hash_boolean(hash, "unique", index.unique);
    hash_text(hash, "origin", index.origin);
    hash_boolean(hash, "partial", index.partial);
    hash_optional_sql(hash, "sql", index.create_sql);
    for (const auto& column : index.columns) {
      hash_integer(hash, "sequence", column.sequence);
      hash_integer(hash, "table_column", column.table_column_rank);
      hash_boolean(hash, "name_present", column.name.has_value());
      hash_text(hash, "name", optional_key(column.name));
      hash_boolean(hash, "descending", column.descending);
      hash_boolean(hash, "collation_present", column.collation.has_value());
      hash_text(hash, "collation", optional_key(column.collation));
      hash_boolean(hash, "key", column.key);
    }
  }
  for (const auto& object : schema.objects) {
    hash_text(hash, "object", object.kind == SchemaObjectKind::view ? "view" : "trigger");
    hash_text(hash, "name", object.name);
    hash_text(hash, "table", object.table);
    hash_text(hash, "sql", canonicalize_sql(object.create_sql));
  }
  return hash.finish_hex();
}

[[nodiscard]] SchemaSnapshot inspect_database(sqlite3* database) {
  {
    Statement attached{database, "PRAGMA database_list;"};
    while (attached.step() == SQLITE_ROW) {
      const auto name = attached.text(1);
      if (name != "main" && name != "temp") {
        throw_unsupported("attached SQLite database is not supported: " + name);
      }
    }
  }
  if (query_integer(database, "SELECT count(*) FROM temp.sqlite_schema;") != 0) {
    throw_unsupported("temporary SQLite schema objects are not supported");
  }

  const auto definitions = schema_sql(database);
  for (const auto& [identity, definition] : definitions) {
    static_cast<void>(definition);
    const auto& [type, name] = identity;
    if (is_dbdiff_reserved_name(name) && !(type == "table" && is_history_table(name))) {
      throw_unsupported("unrecognized reserved SQLite schema object: " + name);
    }
  }
  SchemaSnapshot snapshot;
  Statement tables{database, "SELECT name,type,wr,strict FROM pragma_table_list "
                             "WHERE schema='main' ORDER BY name COLLATE BINARY;"};
  while (tables.step() == SQLITE_ROW) {
    const auto name = tables.text(0);
    const auto type = tables.text(1);
    if (type == "virtual" || type == "shadow") {
      throw_unsupported("virtual and shadow SQLite tables are not supported: " + name);
    }
    if (type != "table" || starts_with_reserved_name(name)) {
      continue;
    }
    const auto definition = definitions.find({"table", name});
    if (definition == definitions.end() || !definition->second.has_value()) {
      throw_unsupported("SQLite table has no inspectable CREATE statement: " + name);
    }
    snapshot.tables.push_back(TableSnapshot{name, definition->second.value_or(std::string{}),
                                            tables.integer(2) != 0, tables.integer(3) != 0,
                                            inspect_columns(database, name),
                                            inspect_foreign_keys(database, name)});
  }
  stable_sort(snapshot.tables, [](const TableSnapshot& table) { return table.name; });
  snapshot.indexes = inspect_indexes(database, snapshot.tables, definitions);

  Statement objects{database, "SELECT type,name,tbl_name,sql FROM main.sqlite_schema "
                              "WHERE type IN ('view','trigger') "
                              "ORDER BY type COLLATE BINARY,name COLLATE BINARY;"};
  while (objects.step() == SQLITE_ROW) {
    const auto type = objects.text(0);
    const auto name = objects.text(1);
    if (starts_with_reserved_name(name)) {
      continue;
    }
    const auto sql = objects.optional_text(3);
    if (!sql.has_value()) {
      throw_unsupported("SQLite schema object has no inspectable CREATE statement: " + name);
    }
    snapshot.objects.push_back(
        SchemaObjectSnapshot{type == "view" ? SchemaObjectKind::view : SchemaObjectKind::trigger,
                             name, objects.text(2), *sql});
  }
  std::ranges::sort(snapshot.objects,
                    [](const SchemaObjectSnapshot& left, const SchemaObjectSnapshot& right) {
                      return std::tuple{left.kind, ascii_lower(left.name), left.name} <
                             std::tuple{right.kind, ascii_lower(right.name), right.name};
                    });
  snapshot.semantic_hash = semantic_hash(snapshot);
  return snapshot;
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

struct TableDefinition {
  std::size_t open_parenthesis{0};
  std::size_t close_parenthesis{0};
  std::vector<std::string> columns;
  std::vector<std::string> constraints;
  std::string trailing;
};

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

[[nodiscard]] bool same_optional_sql(const std::optional<std::string>& left,
                                     const std::optional<std::string>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left.has_value() || canonicalize_sql(*left) == canonicalize_sql(*right);
}

[[nodiscard]] bool same_column(const ColumnSnapshot& left, const ColumnSnapshot& right) {
  return left.rank == right.rank && left.name == right.name &&
         canonicalize_sql(left.declared_type) == canonicalize_sql(right.declared_type) &&
         left.not_null == right.not_null &&
         same_optional_sql(left.default_sql, right.default_sql) &&
         left.primary_key_ordinal == right.primary_key_ordinal && left.generated == right.generated;
}

[[nodiscard]] bool same_table(const TableSnapshot& left, const TableSnapshot& right) {
  if (left.name != right.name || left.without_rowid != right.without_rowid ||
      left.strict != right.strict || left.foreign_keys != right.foreign_keys ||
      canonicalize_sql(left.create_sql) != canonicalize_sql(right.create_sql) ||
      left.columns.size() != right.columns.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.columns.size(); ++index) {
    if (!same_column(left.columns[index], right.columns[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool same_index(const IndexSnapshot& left, const IndexSnapshot& right) {
  return left.name == right.name && left.table == right.table && left.unique == right.unique &&
         left.origin == right.origin && left.partial == right.partial &&
         same_optional_sql(left.create_sql, right.create_sql) && left.columns == right.columns;
}

[[nodiscard]] bool same_object(const SchemaObjectSnapshot& left,
                               const SchemaObjectSnapshot& right) {
  return left.kind == right.kind && left.name == right.name && left.table == right.table &&
         canonicalize_sql(left.create_sql) == canonicalize_sql(right.create_sql);
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

[[nodiscard]] bool safe_added_column(const ColumnSnapshot& column,
                                     const std::string_view definition) {
  if (column.generated != GeneratedColumnKind::ordinary || column.primary_key_ordinal != 0) {
    return false;
  }
  for (const std::string_view keyword : {"primary", "unique", "check", "references", "generated"}) {
    if (contains_unquoted_word(definition, keyword)) {
      return false;
    }
  }
  if (column.default_sql.has_value()) {
    const auto value = trim_sql(*column.default_sql);
    const auto key = ascii_lower(value);
    if (value.starts_with('(') || key == "current_time" || key == "current_date" ||
        key == "current_timestamp") {
      return false;
    }
  }
  if (column.not_null &&
      (!column.default_sql.has_value() || ascii_lower(trim_sql(*column.default_sql)) == "null")) {
    return false;
  }
  return true;
}

[[nodiscard]] std::optional<std::vector<std::string>>
safe_appended_columns(const TableSnapshot& from, const TableSnapshot& to) {
  if (from.name != to.name || from.without_rowid != to.without_rowid || from.strict != to.strict ||
      from.columns.size() >= to.columns.size() || from.foreign_keys != to.foreign_keys) {
    return std::nullopt;
  }
  const auto from_definition = parse_table_definition(from);
  const auto to_definition = parse_table_definition(to);
  if (from_definition.constraints.size() != to_definition.constraints.size() ||
      canonicalize_sql(from_definition.trailing) != canonicalize_sql(to_definition.trailing)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < from_definition.constraints.size(); ++index) {
    if (canonicalize_sql(from_definition.constraints[index]) !=
        canonicalize_sql(to_definition.constraints[index])) {
      return std::nullopt;
    }
  }
  for (std::size_t index = 0; index < from.columns.size(); ++index) {
    if (!same_column(from.columns[index], to.columns[index]) ||
        canonicalize_sql(from_definition.columns[index]) !=
            canonicalize_sql(to_definition.columns[index])) {
      return std::nullopt;
    }
  }

  std::vector<std::string> additions;
  for (std::size_t index = from.columns.size(); index < to.columns.size(); ++index) {
    if (!safe_added_column(to.columns[index], to_definition.columns[index])) {
      return std::nullopt;
    }
    additions.push_back(to_definition.columns[index]);
  }
  return additions;
}

using TableMap = std::map<std::string, const TableSnapshot*>;
using IndexMap = std::map<std::string, const IndexSnapshot*>;
using ObjectKey = std::pair<SchemaObjectKind, std::string>;
using ObjectMap = std::map<ObjectKey, const SchemaObjectSnapshot*>;

[[nodiscard]] TableMap table_map(const SchemaSnapshot& schema) {
  TableMap result;
  for (const auto& table : schema.tables) {
    result.emplace(ascii_lower(table.name), &table);
  }
  return result;
}

[[nodiscard]] IndexMap explicit_index_map(const SchemaSnapshot& schema) {
  IndexMap result;
  for (const auto& index : schema.indexes) {
    if (index.origin == "c" && index.create_sql.has_value()) {
      result.emplace(ascii_lower(index.name), &index);
    }
  }
  return result;
}

[[nodiscard]] ObjectMap object_map(const SchemaSnapshot& schema) {
  ObjectMap result;
  for (const auto& object : schema.objects) {
    result.emplace(ObjectKey{object.kind, ascii_lower(object.name)}, &object);
  }
  return result;
}

[[nodiscard]] bool table_has_primary_key_index(const SchemaSnapshot& schema,
                                               const TableSnapshot& table) {
  const auto table_key = ascii_lower(table.name);
  return std::ranges::any_of(schema.indexes, [&table_key](const IndexSnapshot& index) {
    return ascii_lower(index.table) == table_key && index.origin == "pk";
  });
}

[[nodiscard]] std::optional<std::string> integer_primary_key_alias(const SchemaSnapshot& schema,
                                                                   const TableSnapshot& table) {
  if (table.without_rowid || table_has_primary_key_index(schema, table)) {
    return std::nullopt;
  }
  const ColumnSnapshot* candidate = nullptr;
  for (const auto& column : table.columns) {
    if (column.primary_key_ordinal <= 0) {
      continue;
    }
    if (candidate != nullptr || column.primary_key_ordinal != 1 ||
        canonicalize_sql(column.declared_type) != canonicalize_sql("INTEGER")) {
      return std::nullopt;
    }
    candidate = &column;
  }
  if (candidate == nullptr) {
    return std::nullopt;
  }
  return candidate->name;
}

[[nodiscard]] std::optional<std::string> accessible_rowid_name(const TableSnapshot& table) {
  std::set<std::string> column_names;
  for (const auto& column : table.columns) {
    column_names.insert(ascii_lower(column.name));
  }
  for (const std::string_view candidate : {"rowid", "_rowid_", "oid"}) {
    if (!column_names.contains(std::string{candidate})) {
      return std::string{candidate};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string temporary_table_name(const SchemaSnapshot& from, const SchemaSnapshot& to,
                                               const std::string_view table) {
  std::set<std::string> existing;
  for (const auto& candidate : from.tables) {
    existing.insert(ascii_lower(candidate.name));
  }
  for (const auto& candidate : to.tables) {
    existing.insert(ascii_lower(candidate.name));
  }

  const auto base = "__dbdiff_new_" + sha256_hex(ascii_lower(table)).substr(0, 12U);
  auto candidate = base;
  std::size_t suffix = 1U;
  while (existing.contains(ascii_lower(candidate))) {
    candidate = base + "_" + std::to_string(suffix++);
  }
  return candidate;
}

[[nodiscard]] std::string create_table_as(const TableSnapshot& table, const std::string_view name) {
  const auto definition = parse_table_definition(table);
  std::string result{"CREATE TABLE "};
  result.append(quote_identifier(name));
  result.append(table.create_sql.substr(definition.open_parenthesis));
  return terminated_statement(result);
}

struct CopyTerm {
  std::string destination;
  std::string source_expression;
};

[[nodiscard]] bool
destination_required_without_value(const ColumnSnapshot& destination,
                                   const std::optional<std::string>& target_ipk) {
  if (!destination.not_null || destination.default_sql.has_value()) {
    return false;
  }
  return !target_ipk.has_value() || ascii_lower(*target_ipk) != ascii_lower(destination.name);
}

void append_sequence_preservation(std::vector<std::string>& statements,
                                  const std::string_view source_table,
                                  const std::string_view temporary_table) {
  const auto source = quote_literal(source_table);
  const auto temporary = quote_literal(temporary_table);

  statements.push_back("UPDATE sqlite_sequence SET seq=max(seq,(SELECT seq FROM "
                       "sqlite_sequence WHERE name=" +
                       source + ")) WHERE name=" + temporary +
                       " AND EXISTS(SELECT 1 FROM sqlite_sequence WHERE name=" + source + ");");
  statements.push_back("INSERT INTO sqlite_sequence(name,seq) SELECT " + temporary +
                       ",seq FROM sqlite_sequence WHERE name=" + source +
                       " AND NOT EXISTS(SELECT 1 FROM sqlite_sequence WHERE name=" + temporary +
                       ");");
}

void append_rebuild(std::vector<std::string>& statements, Plan& result,
                    const SchemaSnapshot& from_schema, const SchemaSnapshot& to_schema,
                    const TableSnapshot& from, const TableSnapshot& to) {
  result.hazards.insert(Hazard::table_rewrite);
  result.hazards.insert(Hazard::constraint_scan);
  const auto temporary = temporary_table_name(from_schema, to_schema, to.name);
  statements.push_back(create_table_as(to, temporary));

  std::map<std::string, const ColumnSnapshot*> source_columns;
  for (const auto& column : from.columns) {
    if (column.generated == GeneratedColumnKind::ordinary) {
      source_columns.emplace(ascii_lower(column.name), &column);
    }
  }
  std::set<std::string> destination_columns;
  std::vector<CopyTerm> copy;
  const auto target_ipk = integer_primary_key_alias(to_schema, to);
  for (const auto& column : to.columns) {
    if (column.generated != GeneratedColumnKind::ordinary) {
      continue;
    }
    const auto key = ascii_lower(column.name);
    destination_columns.insert(key);
    const auto source = source_columns.find(key);
    if (source != source_columns.end()) {
      copy.push_back(CopyTerm{column.name, quote_identifier(source->second->name)});
    } else if (destination_required_without_value(column, target_ipk)) {
      result.draft = true;
    }
  }
  for (const auto& [key, column] : source_columns) {
    static_cast<void>(column);
    if (!destination_columns.contains(key)) {
      result.hazards.insert(Hazard::data_loss);
    }
  }

  if (from.without_rowid != to.without_rowid) {
    result.hazards.insert(Hazard::rowid_reassignment);
  } else if (!from.without_rowid) {
    const auto source_ipk = integer_primary_key_alias(from_schema, from);
    const auto same_ipk = source_ipk.has_value() && target_ipk.has_value() &&
                          ascii_lower(*source_ipk) == ascii_lower(*target_ipk);
    if (!same_ipk && !target_ipk.has_value()) {
      const auto source_rowid = source_ipk.has_value() ? source_ipk : accessible_rowid_name(from);
      const auto destination_rowid = accessible_rowid_name(to);
      if (source_rowid.has_value() && destination_rowid.has_value()) {
        copy.insert(copy.begin(), CopyTerm{*destination_rowid, quote_identifier(*source_rowid)});
      } else {
        result.hazards.insert(Hazard::rowid_reassignment);
      }
    } else if (!same_ipk) {
      result.hazards.insert(Hazard::rowid_reassignment);
    }
  }

  if (copy.empty()) {
    result.draft = true;
  } else {
    std::string insert{"INSERT INTO "};
    insert.append(quote_identifier(temporary));
    insert.push_back('(');
    for (std::size_t index = 0; index < copy.size(); ++index) {
      if (index != 0U) {
        insert.push_back(',');
      }
      insert.append(quote_identifier(copy[index].destination));
    }
    insert.append(") SELECT ");
    for (std::size_t index = 0; index < copy.size(); ++index) {
      if (index != 0U) {
        insert.push_back(',');
      }
      insert.append(copy[index].source_expression);
    }
    insert.append(" FROM ");
    insert.append(quote_identifier(from.name));
    insert.push_back(';');
    statements.push_back(std::move(insert));
  }

  const auto target_autoincrement = contains_unquoted_word(to.create_sql, "autoincrement");
  if (target_autoincrement) {
    append_sequence_preservation(statements, from.name, temporary);
  }
  statements.push_back("DROP TABLE " + quote_identifier(from.name) + ";");
  statements.push_back("ALTER TABLE " + quote_identifier(temporary) + " RENAME TO " +
                       quote_identifier(to.name) + ";");
  if (target_autoincrement) {
    auto final_update = "UPDATE sqlite_sequence SET name=" + quote_literal(to.name) +
                        " WHERE name=" + quote_literal(temporary) + ";";
    statements.push_back(std::move(final_update));
  }
}

enum class TableChangeKind : unsigned char { append_columns, rebuild };

struct TableChange {
  const TableSnapshot* from{nullptr};
  const TableSnapshot* to{nullptr};
  TableChangeKind kind{TableChangeKind::rebuild};
  std::vector<std::string> additions;
};

[[nodiscard]] bool key_in(const std::set<std::string>& keys, const std::string_view name) {
  return keys.contains(ascii_lower(name));
}

void append_drop_objects(std::vector<std::string>& statements,
                         const std::vector<const SchemaObjectSnapshot*>& objects,
                         const SchemaObjectKind kind) {
  for (const auto* object : objects) {
    if (object->kind != kind) {
      continue;
    }
    const auto type = kind == SchemaObjectKind::view ? "VIEW " : "TRIGGER ";
    statements.push_back("DROP " + std::string{type} + quote_identifier(object->name) + ";");
  }
}

void append_create_objects(std::vector<std::string>& statements,
                           const std::vector<const SchemaObjectSnapshot*>& objects,
                           const SchemaObjectKind kind) {
  for (const auto* object : objects) {
    if (object->kind == kind) {
      statements.push_back(terminated_statement(object->create_sql));
    }
  }
}

[[nodiscard]] std::vector<const SchemaObjectSnapshot*>
ordered_objects(const SchemaSnapshot& schema) {
  std::vector<const SchemaObjectSnapshot*> result;
  result.reserve(schema.objects.size());
  for (const auto& object : schema.objects) {
    result.push_back(&object);
  }
  std::ranges::sort(result, [](const auto* left, const auto* right) {
    return std::tuple{left->kind, ascii_lower(left->name), left->name} <
           std::tuple{right->kind, ascii_lower(right->name), right->name};
  });
  return result;
}

[[nodiscard]] Plan make_plan(const SchemaSnapshot& from, const SchemaSnapshot& to) {
  Plan result;
  const auto from_tables = table_map(from);
  const auto to_tables = table_map(to);
  std::vector<const TableSnapshot*> removed_tables;
  std::vector<const TableSnapshot*> added_tables;
  std::vector<TableChange> changed_tables;
  std::set<std::string> destructive_tables;
  std::set<std::string> rebuilt_tables;
  std::set<std::string> added_table_keys;

  for (const auto& [key, table] : from_tables) {
    const auto desired = to_tables.find(key);
    if (desired == to_tables.end()) {
      removed_tables.push_back(table);
      destructive_tables.insert(key);
      result.hazards.insert(Hazard::data_loss);
      continue;
    }
    if (same_table(*table, *desired->second)) {
      continue;
    }
    if (auto additions = safe_appended_columns(*table, *desired->second); additions.has_value()) {
      changed_tables.push_back(
          TableChange{table, desired->second, TableChangeKind::append_columns, *additions});
    } else {
      changed_tables.push_back(TableChange{table, desired->second, TableChangeKind::rebuild, {}});
      destructive_tables.insert(key);
      rebuilt_tables.insert(key);
    }
  }
  for (const auto& [key, table] : to_tables) {
    if (!from_tables.contains(key)) {
      added_tables.push_back(table);
      added_table_keys.insert(key);
    }
  }

  const auto from_indexes = explicit_index_map(from);
  const auto to_indexes = explicit_index_map(to);
  const auto from_objects = object_map(from);
  const auto to_objects = object_map(to);
  const auto rebuilds_schema_references = !destructive_tables.empty();
  std::vector<std::string> statements;

  if (rebuilds_schema_references) {
    const auto current_objects = ordered_objects(from);
    append_drop_objects(statements, current_objects, SchemaObjectKind::trigger);
    append_drop_objects(statements, current_objects, SchemaObjectKind::view);
  } else {
    std::vector<const SchemaObjectSnapshot*> drops;
    for (const auto& [key, object] : from_objects) {
      const auto desired = to_objects.find(key);
      if (desired == to_objects.end() || !same_object(*object, *desired->second)) {
        drops.push_back(object);
      }
    }
    append_drop_objects(statements, drops, SchemaObjectKind::trigger);
    append_drop_objects(statements, drops, SchemaObjectKind::view);
  }

  for (const auto& [key, index] : from_indexes) {
    if (key_in(destructive_tables, index->table)) {
      continue;
    }
    const auto desired = to_indexes.find(key);
    if (desired == to_indexes.end() || !same_index(*index, *desired->second)) {
      statements.push_back("DROP INDEX " + quote_identifier(index->name) + ";");
    }
  }

  for (const auto* table : removed_tables) {
    statements.push_back("DROP TABLE " + quote_identifier(table->name) + ";");
  }
  for (const auto& change : changed_tables) {
    if (change.kind == TableChangeKind::append_columns) {
      for (const auto& addition : change.additions) {
        statements.push_back("ALTER TABLE " + quote_identifier(change.from->name) + " ADD COLUMN " +
                             addition + ";");
      }
    } else {
      append_rebuild(statements, result, from, to, *change.from, *change.to);
    }
  }
  for (const auto* table : added_tables) {
    statements.push_back(terminated_statement(table->create_sql));
  }

  for (const auto& [key, index] : to_indexes) {
    const auto current = from_indexes.find(key);
    const auto table_recreated =
        key_in(rebuilt_tables, index->table) || key_in(added_table_keys, index->table);
    if (table_recreated || current == from_indexes.end() || !same_index(*current->second, *index)) {
      statements.push_back(terminated_statement(index->create_sql.value_or(std::string{})));
    }
  }

  if (rebuilds_schema_references) {
    const auto desired_objects = ordered_objects(to);
    append_create_objects(statements, desired_objects, SchemaObjectKind::view);
    append_create_objects(statements, desired_objects, SchemaObjectKind::trigger);
  } else {
    std::vector<const SchemaObjectSnapshot*> creates;
    for (const auto& [key, object] : to_objects) {
      const auto current = from_objects.find(key);
      if (current == from_objects.end() || !same_object(*current->second, *object)) {
        creates.push_back(object);
      }
    }
    append_create_objects(statements, creates, SchemaObjectKind::view);
    append_create_objects(statements, creates, SchemaObjectKind::trigger);
  }

  if (statements.empty()) {
    return result;
  }
  result.hazards.insert(Hazard::write_lock);
  std::string sql;
  if (result.draft) {
    sql.append("-- dbdiff:draft\n");
  }
  if (rebuilds_schema_references) {
    sql.append("PRAGMA foreign_keys=OFF;\n");
  }
  sql.append("BEGIN IMMEDIATE;\n");
  for (const auto& statement : statements) {
    sql.append(statement);
    sql.push_back('\n');
  }
  if (rebuilds_schema_references) {
    sql.append("PRAGMA foreign_key_check;\n");
  }
  sql.append("COMMIT;\n");
  if (rebuilds_schema_references) {
    sql.append("PRAGMA foreign_keys=ON;\n");
  }
  result.sql = std::move(sql);
  return result;
}

[[nodiscard]] std::string render_schema_snapshot(const SchemaSnapshot& snapshot) {
  std::vector<std::string> statements;
  statements.reserve(snapshot.tables.size() + snapshot.indexes.size() + snapshot.objects.size());
  for (const auto& table : snapshot.tables) {
    statements.push_back(terminated_statement(table.create_sql));
  }
  for (const auto& index : snapshot.indexes) {
    if (index.origin == "c" && index.create_sql.has_value()) {
      statements.push_back(terminated_statement(*index.create_sql));
    }
  }
  const auto objects = ordered_objects(snapshot);
  append_create_objects(statements, objects, SchemaObjectKind::view);
  append_create_objects(statements, objects, SchemaObjectKind::trigger);
  if (statements.empty()) {
    return {};
  }

  std::string result{"BEGIN IMMEDIATE;\n"};
  for (const auto& statement : statements) {
    result.append(statement);
    result.push_back('\n');
  }
  result.append("PRAGMA foreign_key_check;\nCOMMIT;\n");
  return result;
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

void restore_foreign_keys(sqlite3* database, const int enabled) {
  if (sqlite3_get_autocommit(database) == 0) {
    execute_one(database, "ROLLBACK;");
  }
  execute_one(database, enabled != 0 ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;");
}

void execute_cleanup(sqlite3* database, const char* sql) noexcept {
  sqlite3_stmt* statement = nullptr;
  const char* tail = nullptr;
  if (sqlite3_prepare_v3(database, sql, -1, 0, &statement, &tail) == SQLITE_OK &&
      statement != nullptr) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
    }
  }
  if (statement != nullptr) {
    static_cast<void>(sqlite3_finalize(statement));
  }
}

void rollback_noexcept(sqlite3* database) noexcept {
  if (sqlite3_get_autocommit(database) == 0) {
    execute_cleanup(database, "ROLLBACK;");
  }
}

void restore_foreign_keys_noexcept(sqlite3* database, const int enabled) noexcept {
  rollback_noexcept(database);
  execute_cleanup(database, enabled != 0 ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;");
}

[[noreturn]] void throw_history_corruption(const std::string& message) {
  throw Error{ErrorCode::database, "invalid dbdiff SQLite migration history: " + message};
}

[[nodiscard]] bool is_lower_sha256(const std::string_view value) {
  return value.size() == 64U &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

[[nodiscard]] std::size_t checked_ordinal(const sqlite3_int64 value) {
  if (value < 0 || static_cast<unsigned long long>(value) >
                       static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
    throw_history_corruption("unit or revision ordinal is out of range");
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] bool history_storage_exists(sqlite3* database) {
  const auto count = query_integer(
      database, "SELECT count(*) FROM main.sqlite_schema "
                "WHERE lower(name) IN "
                "('_dbdiff_migrations','_dbdiff_migration_revisions','_dbdiff_migration_units');");
  if (count == 0) {
    return false;
  }
  if (count != 3) {
    throw_history_corruption("metadata tables are incomplete");
  }
  return true;
}

template <typename Function> void internal_transaction(sqlite3* database, Function&& function) {
  if (sqlite3_get_autocommit(database) == 0) {
    throw_history_corruption("an internal transaction was requested inside an active transaction");
  }
  execute_one(database, "BEGIN IMMEDIATE;");
  try {
    function();
    execute_one(database, "COMMIT;");
  } catch (...) {
    rollback_noexcept(database);
    throw;
  }
}

void ensure_history_storage(sqlite3* database) {
  internal_transaction(database, [database] {
    if (history_storage_exists(database)) {
      return;
    }
    execute_one(database, "CREATE TABLE main.\"_dbdiff_migrations\"("
                          "version TEXT PRIMARY KEY,"
                          "backend TEXT NOT NULL CHECK(backend='sqlite'),"
                          "engine_version TEXT NOT NULL,"
                          "attempted_file_sha256 TEXT NOT NULL,"
                          "completed_file_sha256 TEXT"
                          ") STRICT;");
    execute_one(database, "CREATE TABLE main.\"_dbdiff_migration_revisions\"("
                          "version TEXT NOT NULL,"
                          "revision_ordinal INTEGER NOT NULL CHECK(revision_ordinal>=0),"
                          "exact_file_sha256 TEXT NOT NULL,"
                          "exact_sql BLOB NOT NULL,"
                          "PRIMARY KEY(version,revision_ordinal),"
                          "UNIQUE(version,exact_file_sha256),"
                          "FOREIGN KEY(version) REFERENCES \"_dbdiff_migrations\"(version) "
                          "ON DELETE RESTRICT"
                          ") STRICT;");
    execute_one(database, "CREATE TABLE main.\"_dbdiff_migration_units\"("
                          "version TEXT NOT NULL,"
                          "ordinal INTEGER NOT NULL CHECK(ordinal>=0),"
                          "exact_sha256 TEXT NOT NULL,"
                          "explicit_transaction INTEGER NOT NULL "
                          "CHECK(explicit_transaction IN (0,1)),"
                          "before_schema_sha256 TEXT NOT NULL,"
                          "after_schema_sha256 TEXT NOT NULL,"
                          "completed_revision_sha256 TEXT NOT NULL,"
                          "state TEXT NOT NULL CHECK(state IN ('started','completed')),"
                          "PRIMARY KEY(version,ordinal),"
                          "FOREIGN KEY(version) REFERENCES \"_dbdiff_migrations\"(version) "
                          "ON DELETE RESTRICT"
                          ") STRICT;");
  });
}

[[nodiscard]] MigrationHistory read_history_database(sqlite3* database) {
  if (!history_storage_exists(database)) {
    return {};
  }

  MigrationHistory history;
  history.initialized = true;
  Statement migrations{
      database, "SELECT version,backend,engine_version,attempted_file_sha256,completed_file_sha256 "
                "FROM main.\"_dbdiff_migrations\" ORDER BY version COLLATE BINARY;"};
  while (migrations.step() == SQLITE_ROW) {
    MigrationHistoryEntry entry;
    entry.version = migrations.text(0);
    entry.backend = migrations.text(1);
    entry.engine_version = migrations.text(2);
    entry.attempted_file_sha256 = migrations.text(3);
    entry.completed_file_sha256 = migrations.optional_text(4);
    if (entry.version.empty() || entry.backend != "sqlite" || entry.engine_version.empty() ||
        !is_lower_sha256(entry.attempted_file_sha256) ||
        (entry.completed_file_sha256.has_value() &&
         !is_lower_sha256(*entry.completed_file_sha256))) {
      throw_history_corruption("migration row contains invalid values");
    }

    Statement units{database,
                    "SELECT ordinal,exact_sha256,explicit_transaction,before_schema_sha256,"
                    "after_schema_sha256,state FROM main.\"_dbdiff_migration_units\" "
                    "WHERE version=?1 ORDER BY ordinal;"};
    units.bind_text(1, entry.version);
    bool saw_started = false;
    while (units.step() == SQLITE_ROW) {
      MigrationUnitRecord unit;
      unit.ordinal = checked_ordinal(units.integer64(0));
      unit.exact_sha256 = units.text(1);
      const auto explicit_transaction = units.integer(2);
      unit.explicit_transaction = explicit_transaction != 0;
      unit.before_schema_sha256 = units.text(3);
      unit.after_schema_sha256 = units.text(4);
      const auto state = units.text(5);
      if (unit.ordinal != entry.units.size() ||
          (explicit_transaction != 0 && explicit_transaction != 1) ||
          !is_lower_sha256(unit.exact_sha256) || !is_lower_sha256(unit.before_schema_sha256) ||
          !is_lower_sha256(unit.after_schema_sha256)) {
        throw_history_corruption("migration unit row contains invalid values");
      }
      if (state == "started") {
        if (saw_started) {
          throw_history_corruption("migration contains multiple started units");
        }
        saw_started = true;
        unit.state = MigrationUnitState::started;
      } else if (state == "completed") {
        if (saw_started) {
          throw_history_corruption("a completed unit follows a started unit");
        }
        unit.state = MigrationUnitState::completed;
      } else {
        throw_history_corruption("migration unit has an unknown state");
      }
      entry.units.push_back(std::move(unit));
    }
    if (entry.completed_file_sha256.has_value() && saw_started) {
      throw_history_corruption("a completed migration contains a started unit");
    }
    history.entries.push_back(std::move(entry));
  }

  Statement revisions{database, "SELECT version,revision_ordinal,exact_file_sha256,exact_sql "
                                "FROM main.\"_dbdiff_migration_revisions\" WHERE 0;"};
  if (revisions.step() != SQLITE_DONE) {
    throw_history_corruption("revision metadata validation returned rows");
  }
  return history;
}

void require_history_inputs(const std::string_view version,
                            const std::string_view exact_file_sha256, const std::string_view sql) {
  if (version.empty() || version.size() > 512U || version.find('\0') != std::string_view::npos) {
    throw_migration("migration version must be a non-empty value without NUL bytes");
  }
  if (!is_lower_sha256(exact_file_sha256)) {
    throw_migration("migration file checksum must be a lowercase SHA-256 digest");
  }
  if (sha256_hex(sql) != exact_file_sha256) {
    throw_migration("migration file checksum does not match its exact SQL bytes");
  }
}

void record_attempt(sqlite3* database, const std::string_view version,
                    const std::string_view exact_file_sha256, const std::string_view sql,
                    const bool existing) {
  internal_transaction(database, [&] {
    if (existing) {
      Statement update{database, "UPDATE main.\"_dbdiff_migrations\" "
                                 "SET engine_version=?2,attempted_file_sha256=?3 "
                                 "WHERE version=?1 AND completed_file_sha256 IS NULL;"};
      update.bind_text(1, version);
      update.bind_text(2, sqlite3_libversion());
      update.bind_text(3, exact_file_sha256);
      if (update.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
        throw_history_corruption("could not update the incomplete migration attempt");
      }
    } else {
      Statement insert{database, "INSERT INTO main.\"_dbdiff_migrations\"("
                                 "version,backend,engine_version,attempted_file_sha256) "
                                 "VALUES(?1,'sqlite',?2,?3);"};
      insert.bind_text(1, version);
      insert.bind_text(2, sqlite3_libversion());
      insert.bind_text(3, exact_file_sha256);
      if (insert.step() != SQLITE_DONE) {
        throw_history_corruption("could not insert the migration attempt");
      }
    }

    Statement revision{database,
                       "INSERT INTO main.\"_dbdiff_migration_revisions\"("
                       "version,revision_ordinal,exact_file_sha256,exact_sql) "
                       "SELECT ?1,COALESCE((SELECT max(revision_ordinal)+1 "
                       "FROM main.\"_dbdiff_migration_revisions\" WHERE version=?1),0),?2,?3 "
                       "WHERE NOT EXISTS(SELECT 1 FROM main.\"_dbdiff_migration_revisions\" "
                       "WHERE version=?1 AND exact_file_sha256=?2);"};
    revision.bind_text(1, version);
    revision.bind_text(2, exact_file_sha256);
    revision.bind_blob(3, sql);
    if (revision.step() != SQLITE_DONE) {
      throw_history_corruption("could not store the exact migration revision");
    }
  });
}

void insert_unit_record(sqlite3* database, const std::string_view version,
                        const MigrationUnitRecord& unit,
                        const std::string_view completed_revision_sha256) {
  Statement insert{database,
                   "INSERT INTO main.\"_dbdiff_migration_units\"("
                   "version,ordinal,exact_sha256,explicit_transaction,before_schema_sha256,"
                   "after_schema_sha256,completed_revision_sha256,state) "
                   "VALUES(?1,?2,?3,?4,?5,?6,?7,?8);"};
  insert.bind_text(1, version);
  insert.bind_integer(2, static_cast<sqlite3_int64>(unit.ordinal));
  insert.bind_text(3, unit.exact_sha256);
  insert.bind_integer(4, unit.explicit_transaction ? 1 : 0);
  insert.bind_text(5, unit.before_schema_sha256);
  insert.bind_text(6, unit.after_schema_sha256);
  insert.bind_text(7, completed_revision_sha256);
  insert.bind_text(8, unit.state == MigrationUnitState::completed ? "completed" : "started");
  if (insert.step() != SQLITE_DONE) {
    throw_history_corruption("could not record a migration unit");
  }
}

void insert_unit_record_atomic(sqlite3* database, const std::string_view version,
                               const MigrationUnitRecord& unit,
                               const std::string_view completed_revision_sha256) {
  internal_transaction(
      database, [&] { insert_unit_record(database, version, unit, completed_revision_sha256); });
}

void complete_started_unit(sqlite3* database, const std::string_view version,
                           const std::size_t ordinal) {
  internal_transaction(database, [&] {
    Statement update{database, "UPDATE main.\"_dbdiff_migration_units\" SET state='completed' "
                               "WHERE version=?1 AND ordinal=?2 AND state='started';"};
    update.bind_text(1, version);
    update.bind_integer(2, static_cast<sqlite3_int64>(ordinal));
    if (update.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
      throw_history_corruption("could not complete a started migration unit");
    }
  });
}

void remove_started_unit(sqlite3* database, const std::string_view version,
                         const std::size_t ordinal) {
  internal_transaction(database, [&] {
    Statement remove{database, "DELETE FROM main.\"_dbdiff_migration_units\" "
                               "WHERE version=?1 AND ordinal=?2 AND state='started';"};
    remove.bind_text(1, version);
    remove.bind_integer(2, static_cast<sqlite3_int64>(ordinal));
    if (remove.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
      throw_history_corruption("could not discard a non-applied standalone unit");
    }
  });
}

void complete_migration(sqlite3* database, const std::string_view version,
                        const std::string_view exact_file_sha256) {
  internal_transaction(database, [&] {
    Statement update{database, "UPDATE main.\"_dbdiff_migrations\" SET completed_file_sha256=?2 "
                               "WHERE version=?1 AND completed_file_sha256 IS NULL;"};
    update.bind_text(1, version);
    update.bind_text(2, exact_file_sha256);
    if (update.step() != SQLITE_DONE || sqlite3_changes(database) != 1) {
      throw_history_corruption("could not mark the migration complete");
    }
  });
}

[[nodiscard]] bool is_replayable_session_unit(const ExecutionUnit& unit) {
  return !unit.explicit_transaction && unit.statements.size() == 1U &&
         unit.statements.front().kind == StatementKind::session;
}

void execute_unit_plain(sqlite3* database, const ParsedScript& parsed, const ExecutionUnit& unit) {
  for (const auto& statement : unit.statements) {
    const auto text =
        std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin);
    execute_one(database, text,
                statement.kind == StatementKind::session &&
                    pragma_name(text) == "foreign_key_check");
  }
}

void copy_database(sqlite3* source, sqlite3* destination) {
  auto* backup = sqlite3_backup_init(destination, "main", source, "main");
  if (backup == nullptr) {
    throw_sqlite(destination, sqlite3_errcode(destination), "creating SQLite backup");
  }

  constexpr int pages_per_step = 128;
  constexpr int maximum_busy_retries = 500;
  int busy_retries = 0;
  int step_result = SQLITE_OK;
  while (step_result == SQLITE_OK || step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
    if (step_result == SQLITE_BUSY || step_result == SQLITE_LOCKED) {
      if (++busy_retries > maximum_busy_retries) {
        break;
      }
      static_cast<void>(sqlite3_sleep(10));
    }
    step_result = sqlite3_backup_step(backup, pages_per_step);
  }
  const auto step_error =
      step_result == SQLITE_DONE
          ? std::string{}
          : sqlite_error_message(destination, step_result, "copying SQLite database");
  const auto finish_result = sqlite3_backup_finish(backup);
  if (step_result != SQLITE_DONE) {
    throw Error{ErrorCode::database, step_error};
  }
  if (finish_result != SQLITE_OK) {
    throw_sqlite(destination, finish_result, "finishing SQLite backup");
  }
}

} // namespace

struct Database::Impl {
  Impl(const std::string& locator, const OpenMode mode) {
    const auto result = sqlite3_open_v2(locator.c_str(), &handle, open_flags(mode), nullptr);
    if (result != SQLITE_OK) {
      const auto message = sqlite_error_message(handle, result, "opening database");
      if (handle != nullptr) {
        static_cast<void>(sqlite3_close_v2(handle));
        handle = nullptr;
      }
      throw Error{ErrorCode::database, message};
    }
    try {
      configure_database(handle);
    } catch (...) {
      static_cast<void>(sqlite3_close_v2(handle));
      handle = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (handle != nullptr) {
      static_cast<void>(sqlite3_close_v2(handle));
    }
  }

  sqlite3* handle{nullptr};
};

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

Database::Database(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

Database::~Database() = default;
Database::Database(Database&&) noexcept = default;
Database& Database::operator=(Database&&) noexcept = default;

Database Database::temporary() {
  return Database{std::make_unique<Impl>(std::string{}, OpenMode::read_write_create)};
}

Database Database::open(const std::filesystem::path& path, const OpenMode mode) {
  validate_database_path(path);
  return Database{std::make_unique<Impl>(path.string(), mode)};
}

void Database::execute_source(const std::string_view sql) {
  const auto statements = scan_statements(sql);
  validate_source_statements(sql, statements);

  execute_one(implementation_->handle, "BEGIN IMMEDIATE;");
  try {
    for (const auto& statement : statements) {
      execute_one(implementation_->handle,
                  sql.substr(statement.begin, statement.end - statement.begin));
    }
    static_cast<void>(inspect_database(implementation_->handle));
    execute_one(implementation_->handle, "PRAGMA foreign_key_check;", true);
    execute_one(implementation_->handle, "COMMIT;");
  } catch (...) {
    rollback_noexcept(implementation_->handle);
    throw;
  }
}

void Database::execute_migration(const std::string_view sql) {
  const auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  const auto foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");

  try {
    for (const auto& statement : statements) {
      const auto text = sql.substr(statement.begin, statement.end - statement.begin);
      execute_one(implementation_->handle, text,
                  statement.kind == StatementKind::session &&
                      pragma_name(text) == "foreign_key_check");
    }
    if (sqlite3_get_autocommit(implementation_->handle) == 0) {
      throw Error{ErrorCode::migration,
                  "SQLite migration left the connection inside a transaction"};
    }
    restore_foreign_keys(implementation_->handle, foreign_keys);
    static_cast<void>(inspect_database(implementation_->handle));
  } catch (...) {
    restore_foreign_keys_noexcept(implementation_->handle, foreign_keys);
    throw;
  }
}

void Database::execute_prefix(const std::string_view sql, const std::size_t completed_unit_count) {
  auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  auto parsed = build_execution_units(std::string{sql}, std::move(statements));
  if (completed_unit_count > parsed.units.size()) {
    throw_migration("SQLite migration prefix contains more units than the script");
  }
  const auto foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");
  try {
    for (std::size_t index = 0; index < completed_unit_count; ++index) {
      execute_unit_plain(implementation_->handle, parsed, parsed.units[index]);
    }
    if (sqlite3_get_autocommit(implementation_->handle) == 0) {
      throw_migration("SQLite migration prefix left the connection inside a transaction");
    }
    restore_foreign_keys(implementation_->handle, foreign_keys);
    static_cast<void>(inspect_database(implementation_->handle));
  } catch (...) {
    restore_foreign_keys_noexcept(implementation_->handle, foreign_keys);
    throw;
  }
}

void Database::backup_to(Database& destination) const {
  if (implementation_.get() == destination.implementation_.get()) {
    throw Error{ErrorCode::database, "SQLite backup source and destination must be different"};
  }
  if (sqlite3_get_autocommit(implementation_->handle) == 0 ||
      sqlite3_get_autocommit(destination.implementation_->handle) == 0) {
    throw Error{ErrorCode::database,
                "SQLite online backup requires idle source and destination connections"};
  }
  static_cast<void>(inspect_database(implementation_->handle));
  copy_database(implementation_->handle, destination.implementation_->handle);
  static_cast<void>(inspect_database(destination.implementation_->handle));
}

MigrationHistory Database::read_history() const {
  return read_history_database(implementation_->handle);
}

std::vector<MigrationRevision> Database::recover_revisions(const std::string_view version) const {
  if (!history_storage_exists(implementation_->handle)) {
    return {};
  }
  Statement revisions{implementation_->handle,
                      "SELECT revision_ordinal,exact_file_sha256,exact_sql "
                      "FROM main.\"_dbdiff_migration_revisions\" "
                      "WHERE version=?1 ORDER BY revision_ordinal;"};
  revisions.bind_text(1, version);
  std::vector<MigrationRevision> result;
  while (revisions.step() == SQLITE_ROW) {
    MigrationRevision revision;
    revision.ordinal = checked_ordinal(revisions.integer64(0));
    revision.exact_file_sha256 = revisions.text(1);
    revision.sql = revisions.blob(2);
    if (revision.ordinal != result.size() || !is_lower_sha256(revision.exact_file_sha256) ||
        sha256_hex(revision.sql) != revision.exact_file_sha256) {
      throw_history_corruption("stored migration revision is corrupt");
    }
    result.push_back(std::move(revision));
  }
  return result;
}

MigrationApplyResult Database::apply_version(const std::string_view version,
                                             const std::string_view exact_file_sha256,
                                             const std::string_view sql, const bool resume) {
  require_history_inputs(version, exact_file_sha256, sql);
  auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  auto parsed = build_execution_units(std::string{sql}, std::move(statements));
  if (sqlite3_get_autocommit(implementation_->handle) == 0) {
    throw_migration("cannot apply a SQLite migration inside an active transaction");
  }

  ensure_history_storage(implementation_->handle);
  auto history = read_history_database(implementation_->handle);
  auto found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
  const auto existing = found != history.entries.end();
  if (existing && found->completed_file_sha256.has_value()) {
    if (found->attempted_file_sha256 != *found->completed_file_sha256) {
      throw_history_corruption("completed migration checksum does not match its last attempt");
    }
    if (*found->completed_file_sha256 != exact_file_sha256) {
      throw_migration("completed migration '" + std::string{version} +
                      "' has different exact SQL bytes");
    }
    return MigrationApplyResult{true, found->units.size(), *found->completed_file_sha256};
  }
  if (existing && !resume) {
    throw_migration("migration '" + std::string{version} +
                    "' is incomplete; explicit resume is required");
  }
  if (!existing && resume) {
    throw_migration("migration '" + std::string{version} + "' has no incomplete attempt to resume");
  }

  record_attempt(implementation_->handle, version, exact_file_sha256, parsed.sql, existing);
  history = read_history_database(implementation_->handle);
  found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
  if (found == history.entries.end() || found->completed_file_sha256.has_value()) {
    throw_history_corruption("attempted migration row could not be read back");
  }

  if (!found->units.empty() && found->units.back().state == MigrationUnitState::started) {
    const auto started = found->units.back();
    if (started.explicit_transaction) {
      throw_history_corruption("an explicit transaction unit is only partially recorded");
    }
    const auto current_schema_sha256 = inspect_database(implementation_->handle).semantic_hash;
    if (started.before_schema_sha256 == started.after_schema_sha256) {
      throw_migration("cannot safely reconcile standalone migration unit " +
                      std::to_string(started.ordinal + 1U) +
                      " because its before and after schemas are indistinguishable");
    }
    if (current_schema_sha256 == started.after_schema_sha256) {
      if (started.ordinal >= parsed.units.size() ||
          parsed.units[started.ordinal].exact_sha256 != started.exact_sha256) {
        throw_migration("cannot safely resume an edited standalone migration unit that may "
                        "already be applied");
      }
      complete_started_unit(implementation_->handle, version, started.ordinal);
    } else if (current_schema_sha256 == started.before_schema_sha256) {
      remove_started_unit(implementation_->handle, version, started.ordinal);
    } else {
      throw_migration("cannot safely reconcile standalone migration unit " +
                      std::to_string(started.ordinal + 1U) +
                      "; the schema matches neither its before nor after checkpoint");
    }
    history = read_history_database(implementation_->handle);
    found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
    if (found == history.entries.end()) {
      throw_history_corruption("migration disappeared while reconciling its last unit");
    }
  }

  std::vector<std::string> completed_hashes;
  completed_hashes.reserve(found->units.size());
  for (const auto& unit : found->units) {
    if (unit.state != MigrationUnitState::completed) {
      throw_history_corruption("a started unit remained after reconciliation");
    }
    completed_hashes.push_back(unit.exact_sha256);
  }
  validate_completed_prefix(completed_hashes, parsed);
  for (std::size_t index = 0; index < found->units.size(); ++index) {
    if (found->units[index].explicit_transaction != parsed.units[index].explicit_transaction) {
      throw_history_corruption("stored unit transaction shape differs from its exact SQL");
    }
  }

  const auto completed_unit_count = found->units.size();
  const auto foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");
  try {
    for (std::size_t index = 0; index < completed_unit_count; ++index) {
      if (is_replayable_session_unit(parsed.units[index])) {
        execute_unit_plain(implementation_->handle, parsed, parsed.units[index]);
      }
    }

    for (std::size_t index = completed_unit_count; index < parsed.units.size(); ++index) {
      const auto& execution_unit = parsed.units[index];
      const auto before_schema_sha256 = inspect_database(implementation_->handle).semantic_hash;

      if (is_replayable_session_unit(execution_unit)) {
        execute_unit_plain(implementation_->handle, parsed, execution_unit);
        const auto after_schema_sha256 = inspect_database(implementation_->handle).semantic_hash;
        if (before_schema_sha256 != after_schema_sha256) {
          throw_migration("SQLite session directive unexpectedly changed the persistent schema");
        }
        insert_unit_record_atomic(implementation_->handle, version,
                                  MigrationUnitRecord{index, execution_unit.exact_sha256, false,
                                                      before_schema_sha256, after_schema_sha256,
                                                      MigrationUnitState::completed},
                                  exact_file_sha256);
        continue;
      }

      if (execution_unit.explicit_transaction) {
        bool checkpointed = false;
        for (const auto& statement : execution_unit.statements) {
          const auto text =
              std::string_view{parsed.sql}.substr(statement.begin, statement.end - statement.begin);
          if (statement.kind == StatementKind::commit) {
            const auto after_schema_sha256 =
                inspect_database(implementation_->handle).semantic_hash;
            insert_unit_record(implementation_->handle, version,
                               MigrationUnitRecord{index, execution_unit.exact_sha256, true,
                                                   before_schema_sha256, after_schema_sha256,
                                                   MigrationUnitState::completed},
                               exact_file_sha256);
            execute_one(implementation_->handle, text);
            checkpointed = true;
          } else {
            execute_one(implementation_->handle, text,
                        statement.kind == StatementKind::session &&
                            pragma_name(text) == "foreign_key_check");
          }
        }
        if (!checkpointed || sqlite3_get_autocommit(implementation_->handle) == 0) {
          throw_migration("SQLite explicit transaction unit did not commit cleanly");
        }
        continue;
      }

      auto clone = Database::temporary();
      copy_database(implementation_->handle, clone.implementation_->handle);
      const auto unit_foreign_keys = query_integer(implementation_->handle, "PRAGMA foreign_keys;");
      execute_one(clone.implementation_->handle,
                  unit_foreign_keys != 0 ? "PRAGMA foreign_keys=ON;" : "PRAGMA foreign_keys=OFF;");
      execute_unit_plain(clone.implementation_->handle, parsed, execution_unit);
      const auto expected_after_schema_sha256 = clone.inspect().semantic_hash;
      insert_unit_record_atomic(
          implementation_->handle, version,
          MigrationUnitRecord{index, execution_unit.exact_sha256, false, before_schema_sha256,
                              expected_after_schema_sha256, MigrationUnitState::started},
          exact_file_sha256);
      execute_unit_plain(implementation_->handle, parsed, execution_unit);
      const auto actual_after_schema_sha256 =
          inspect_database(implementation_->handle).semantic_hash;
      if (actual_after_schema_sha256 != expected_after_schema_sha256) {
        throw_migration("standalone SQLite DDL produced a schema different from its preflight");
      }
      complete_started_unit(implementation_->handle, version, index);
    }

    restore_foreign_keys(implementation_->handle, foreign_keys);
    static_cast<void>(inspect_database(implementation_->handle));
    complete_migration(implementation_->handle, version, exact_file_sha256);
  } catch (...) {
    restore_foreign_keys_noexcept(implementation_->handle, foreign_keys);
    throw;
  }
  return MigrationApplyResult{false, parsed.units.size(), std::string{exact_file_sha256}};
}

SchemaSnapshot Database::inspect() const { return inspect_database(implementation_->handle); }

Plan plan(const SchemaSnapshot& from, const SchemaSnapshot& to) { return make_plan(from, to); }

std::string render_snapshot(const SchemaSnapshot& snapshot) {
  return render_schema_snapshot(snapshot);
}

bool validate_plan(const SchemaSnapshot& from, const SchemaSnapshot& to) {
  auto database = Database::temporary();
  const auto baseline = render_schema_snapshot(from);
  if (!baseline.empty()) {
    database.execute_migration(baseline);
  }
  if (database.inspect().semantic_hash != semantic_hash(from)) {
    return false;
  }

  const auto candidate = make_plan(from, to);
  if (!candidate.sql.empty()) {
    database.execute_migration(candidate.sql);
  }
  return database.inspect().semantic_hash == semantic_hash(to);
}

BackendKind kind() noexcept { return BackendKind::sqlite; }

} // namespace dbdiff::sqlite
