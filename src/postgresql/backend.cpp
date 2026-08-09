#include "dbdiff/postgresql.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"

#include <libpq-fe.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dbdiff::postgresql {
namespace {

constexpr int minimum_server_version = 150000;
constexpr int maximum_server_version = 190000;
constexpr std::string_view metadata_schema = "_dbdiff";
constexpr std::string_view scratch_prefix = "dbdiff_";
constexpr std::size_t scratch_token_size = 32;

struct ConninfoDeleter {
  void operator()(PQconninfoOption* value) const noexcept {
    if (value != nullptr) {
      PQconninfoFree(value);
    }
  }
};

struct LibpqMemoryDeleter {
  void operator()(char* value) const noexcept {
    if (value != nullptr) {
      PQfreemem(value);
    }
  }
};

[[nodiscard]] bool contains_nul(const std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

void require_no_nul(const std::string_view value, const std::string_view description) {
  if (contains_nul(value)) {
    throw Error{ErrorCode::configuration, std::string{description} + " must not contain NUL"};
  }
}

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

[[nodiscard]] std::string quote_conninfo_value(const std::string_view value) {
  require_no_nul(value, "PostgreSQL connection option");

  std::string result;
  result.reserve(value.size() + 2);
  result.push_back('\'');
  for (const char character : value) {
    if (character == '\'' || character == '\\') {
      result.push_back('\\');
    }
    result.push_back(character);
  }
  result.push_back('\'');
  return result;
}

[[nodiscard]] bool is_lower_hex_token(const std::string_view token) noexcept {
  return token.size() == scratch_token_size && std::ranges::all_of(token, [](const char character) {
           return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] std::string random_token() {
  std::array<unsigned char, scratch_token_size / 2> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw Error{ErrorCode::database, "could not generate a PostgreSQL scratch identifier"};
  }

  constexpr std::string_view digits = "0123456789abcdef";
  std::string token;
  token.reserve(scratch_token_size);
  for (const unsigned char byte : bytes) {
    token.push_back(digits[static_cast<std::size_t>(byte >> 4U)]);
    token.push_back(digits[static_cast<std::size_t>(byte & 0x0fU)]);
  }
  return token;
}

[[nodiscard]] std::int64_t current_epoch_seconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

[[nodiscard]] TablePersistence parse_persistence(const std::string_view value) {
  if (value == "p") {
    return TablePersistence::permanent;
  }
  if (value == "u") {
    return TablePersistence::unlogged;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL table persistence"};
}

[[nodiscard]] IdentityGeneration parse_identity(const std::string_view value) {
  if (value.empty()) {
    return IdentityGeneration::none;
  }
  if (value == "a") {
    return IdentityGeneration::always;
  }
  if (value == "d") {
    return IdentityGeneration::by_default;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL identity generation mode"};
}

[[nodiscard]] GeneratedStorage parse_generated(const std::string_view value) {
  if (value.empty()) {
    return GeneratedStorage::none;
  }
  if (value == "s") {
    return GeneratedStorage::stored;
  }
  if (value == "v") {
    return GeneratedStorage::virtual_column;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL generated-column mode"};
}

[[nodiscard]] std::vector<std::string>
validate_managed_schemas(const std::vector<std::string>& managed_schemas) {
  std::vector<std::string> schemas = managed_schemas;
  for (const auto& schema : schemas) {
    require_no_nul(schema, "managed schema name");
    if (schema.empty()) {
      throw Error{ErrorCode::configuration, "managed schema name must not be empty"};
    }
    if (schema == metadata_schema) {
      throw Error{ErrorCode::configuration, "the dbdiff metadata schema cannot be managed"};
    }
  }

  std::ranges::sort(schemas);
  if (std::ranges::adjacent_find(schemas) != schemas.end()) {
    throw Error{ErrorCode::configuration, "managed schema names must be unique"};
  }
  return schemas;
}

[[nodiscard]] std::string schema_filter(pqxx::transaction_base& transaction,
                                        const std::vector<std::string>& schemas) {
  std::string filter;
  for (const auto& schema : schemas) {
    if (!filter.empty()) {
      filter += ", ";
    }
    filter += transaction.quote(schema);
  }
  return filter;
}

using SnapshotTransaction =
    pqxx::transaction<pqxx::isolation_level::repeatable_read, pqxx::write_policy::read_only>;

void execute_no_rows(pqxx::transaction_base& transaction, const std::string_view statement) {
#if PQXX_VERSION_MAJOR >= 8
  transaction.exec(statement).no_rows();
#else
  transaction.exec0(std::string{statement});
#endif
}

[[nodiscard]] pqxx::row execute_one_row(pqxx::transaction_base& transaction,
                                        const std::string_view statement) {
#if PQXX_VERSION_MAJOR >= 8
  return transaction.exec(statement).one_row();
#else
  return transaction.exec1(std::string{statement});
#endif
}

[[nodiscard]] pqxx::result execute_one_parameter(pqxx::transaction_base& transaction,
                                                 const std::string_view statement,
                                                 const std::string& parameter) {
#if PQXX_VERSION_MAJOR >= 8
  return transaction.exec(statement, pqxx::params{parameter});
#else
  return transaction.exec_params(std::string{statement}, parameter);
#endif
}

template <typename... Parameters>
[[nodiscard]] pqxx::result execute_parameters(pqxx::transaction_base& transaction,
                                              const std::string_view statement,
                                              Parameters&&... parameters) {
#if PQXX_VERSION_MAJOR >= 8
  return transaction.exec(statement, pqxx::params{std::forward<Parameters>(parameters)...});
#else
  return transaction.exec_params(std::string{statement}, std::forward<Parameters>(parameters)...);
#endif
}

constexpr std::string_view advisory_lock_sql =
    "SELECT pg_catalog.pg_advisory_lock(168430090, 1145194822)";
constexpr std::string_view advisory_unlock_sql =
    "SELECT pg_catalog.pg_advisory_unlock(168430090, 1145194822)";

[[nodiscard]] bool lower_sha256(const std::string_view value) noexcept {
  return value.size() == 64U &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

void require_file_sha256(const std::string_view value) {
  if (!lower_sha256(value)) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration file hash must be a lowercase SHA-256 digest"};
  }
}

void require_version(const std::string_view version) {
  if (version.empty() || version.size() > 512U || contains_nul(version)) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration version must be 1-512 bytes without NUL"};
  }
}

class SessionAdvisoryLock final {
public:
  explicit SessionAdvisoryLock(pqxx::nontransaction& transaction) : transaction_{transaction} {
    static_cast<void>(execute_one_row(transaction_, advisory_lock_sql));
  }

  SessionAdvisoryLock(const SessionAdvisoryLock&) = delete;
  SessionAdvisoryLock& operator=(const SessionAdvisoryLock&) = delete;

  ~SessionAdvisoryLock() {
    try {
      const auto row = execute_one_row(transaction_, advisory_unlock_sql);
      static_cast<void>(row);
    } catch (const std::exception& error) {
      static_cast<void>(error);
    }
  }

private:
  pqxx::nontransaction& transaction_;
};

void ensure_history_tables(pqxx::transaction_base& transaction) {
  execute_no_rows(transaction, "CREATE SCHEMA IF NOT EXISTS \"_dbdiff\"");
  execute_no_rows(
      transaction,
      "CREATE TABLE IF NOT EXISTS \"_dbdiff\".\"migrations\" ("
      "version text PRIMARY KEY,"
      "backend text NOT NULL CHECK (backend = 'postgresql'),"
      "engine_version text NOT NULL,"
      "attempted_file_sha256 text NOT NULL "
      "  CHECK (attempted_file_sha256 ~ '^[0-9a-f]{64}$'),"
      "completed_file_sha256 text "
      "  CHECK (completed_file_sha256 IS NULL OR "
      "         completed_file_sha256 ~ '^[0-9a-f]{64}$'),"
      "created_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp(),"
      "updated_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp()"
      ")");
  execute_no_rows(
      transaction,
      "CREATE TABLE IF NOT EXISTS \"_dbdiff\".\"revisions\" ("
      "version text NOT NULL REFERENCES \"_dbdiff\".\"migrations\"(version) "
      "  ON UPDATE RESTRICT ON DELETE RESTRICT,"
      "ordinal bigint NOT NULL CHECK (ordinal >= 0),"
      "exact_file_sha256 text NOT NULL CHECK (exact_file_sha256 ~ '^[0-9a-f]{64}$'),"
      "sql text NOT NULL,"
      "created_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp(),"
      "PRIMARY KEY (version, ordinal),"
      "UNIQUE (version, exact_file_sha256)"
      ")");
  execute_no_rows(
      transaction,
      "CREATE TABLE IF NOT EXISTS \"_dbdiff\".\"units\" ("
      "version text NOT NULL REFERENCES \"_dbdiff\".\"migrations\"(version) "
      "  ON UPDATE RESTRICT ON DELETE RESTRICT,"
      "ordinal bigint NOT NULL CHECK (ordinal >= 0),"
      "exact_sha256 text NOT NULL CHECK (exact_sha256 ~ '^[0-9a-f]{64}$'),"
      "explicit_transaction boolean NOT NULL,"
      "before_schema_sha256 text NOT NULL "
      "  CHECK (before_schema_sha256 = '' OR before_schema_sha256 ~ '^[0-9a-f]{64}$'),"
      "after_schema_sha256 text NOT NULL "
      "  CHECK (after_schema_sha256 = '' OR after_schema_sha256 ~ '^[0-9a-f]{64}$'),"
      "state text NOT NULL CHECK (state IN ('started', 'completed')),"
      "revision_ordinal bigint NOT NULL CHECK (revision_ordinal >= 0),"
      "updated_at timestamp with time zone NOT NULL DEFAULT pg_catalog.clock_timestamp(),"
      "PRIMARY KEY (version, ordinal),"
      "FOREIGN KEY (version, revision_ordinal) "
      "  REFERENCES \"_dbdiff\".\"revisions\"(version, ordinal) "
      "  ON UPDATE RESTRICT ON DELETE RESTRICT"
      ")");
}

[[nodiscard]] bool history_tables_initialized(pqxx::transaction_base& transaction) {
  const auto row =
      execute_one_row(transaction, "SELECT pg_catalog.to_regclass('_dbdiff.migrations')::text,"
                                   "       pg_catalog.to_regclass('_dbdiff.revisions')::text,"
                                   "       pg_catalog.to_regclass('_dbdiff.units')::text");
  const std::array present{!row[0].is_null(), !row[1].is_null(), !row[2].is_null()};
  const auto count = static_cast<std::size_t>(std::ranges::count(present, true));
  if (count != 0U && count != present.size()) {
    throw Error{ErrorCode::drift, "PostgreSQL migration history tables are incomplete"};
  }
  return count == present.size();
}

template <typename Field>
[[nodiscard]] std::size_t history_ordinal(const Field& field, const std::string_view description) {
  const auto value = field.template as<std::uint64_t>();
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw Error{ErrorCode::drift, std::string{description} + " exceeds this build's size limit"};
  }
  return static_cast<std::size_t>(value);
}

void validate_stored_hash(const std::string_view hash, const std::string_view description) {
  if (!lower_sha256(hash)) {
    throw Error{ErrorCode::drift,
                "PostgreSQL migration history contains an invalid " + std::string{description}};
  }
}

[[nodiscard]] MigrationHistory read_history_unlocked(pqxx::transaction_base& transaction) {
  MigrationHistory history;
  history.initialized = history_tables_initialized(transaction);
  if (!history.initialized) {
    return history;
  }

  const auto migration_rows = transaction.exec(
      "SELECT version, backend, engine_version, attempted_file_sha256, completed_file_sha256 "
      "FROM \"_dbdiff\".\"migrations\" ORDER BY version COLLATE \"C\"");
  std::map<std::string, std::size_t, std::less<>> entry_indexes;
  history.entries.reserve(static_cast<std::size_t>(migration_rows.size()));
  for (const auto& row : migration_rows) {
    auto attempted_hash = row[3].as<std::string>();
    validate_stored_hash(attempted_hash, "attempted file hash");
    std::optional<std::string> completed_hash;
    if (!row[4].is_null()) {
      completed_hash = row[4].as<std::string>();
      validate_stored_hash(*completed_hash, "completed file hash");
    }
    auto version = row[0].as<std::string>();
    const auto backend = row[1].as<std::string>();
    const auto engine_version = row[2].as<std::string>();
    if (version.empty() || contains_nul(version) || backend != "postgresql" ||
        engine_version.empty() ||
        (completed_hash.has_value() && *completed_hash != attempted_hash)) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history contains an invalid migration record"};
    }
    const auto index = history.entries.size();
    if (!entry_indexes.emplace(version, index).second) {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains a duplicate version"};
    }
    history.entries.push_back(MigrationHistoryEntry{
        .version = std::move(version),
        .backend = backend,
        .engine_version = engine_version,
        .attempted_file_sha256 = std::move(attempted_hash),
        .completed_file_sha256 = std::move(completed_hash),
        .units = {},
    });
  }

  const auto unit_rows =
      transaction.exec("SELECT version, ordinal, exact_sha256, explicit_transaction, "
                       "       before_schema_sha256, after_schema_sha256, state "
                       "FROM \"_dbdiff\".\"units\" ORDER BY version COLLATE \"C\", ordinal");
  for (const auto& row : unit_rows) {
    const auto version = row[0].as<std::string>();
    const auto entry = entry_indexes.find(version);
    if (entry == entry_indexes.end()) {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains an orphan unit"};
    }
    auto exact_hash = row[2].as<std::string>();
    validate_stored_hash(exact_hash, "unit hash");
    const auto state_text = row[6].as<std::string>();
    MigrationUnitState state{};
    if (state_text == "started") {
      state = MigrationUnitState::started;
    } else if (state_text == "completed") {
      state = MigrationUnitState::completed;
    } else {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains an invalid unit state"};
    }
    auto& units = history.entries[entry->second].units;
    const auto ordinal = history_ordinal(row[1], "migration unit ordinal");
    const auto before_hash = row[4].as<std::string>();
    const auto after_hash = row[5].as<std::string>();
    if (ordinal != units.size() || !lower_sha256(before_hash) ||
        (state == MigrationUnitState::completed && !lower_sha256(after_hash)) ||
        (state == MigrationUnitState::started &&
         (row[3].as<bool>() || !after_hash.empty() ||
          std::ranges::any_of(units, [](const MigrationUnitRecord& unit) {
            return unit.state == MigrationUnitState::started;
          })))) {
      throw Error{ErrorCode::drift, "PostgreSQL migration history contains an invalid unit record"};
    }
    if (state == MigrationUnitState::completed &&
        std::ranges::any_of(units, [](const MigrationUnitRecord& unit) {
          return unit.state == MigrationUnitState::started;
        })) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history has a completed unit after a started unit"};
    }
    units.push_back(MigrationUnitRecord{
        .ordinal = ordinal,
        .exact_sha256 = std::move(exact_hash),
        .explicit_transaction = row[3].as<bool>(),
        .before_schema_sha256 = before_hash,
        .after_schema_sha256 = after_hash,
        .state = state,
    });
  }
  for (const auto& entry : history.entries) {
    if (entry.completed_file_sha256.has_value() &&
        std::ranges::any_of(entry.units, [](const MigrationUnitRecord& unit) {
          return unit.state == MigrationUnitState::started;
        })) {
      throw Error{ErrorCode::drift, "completed PostgreSQL migration contains a started unit"};
    }
  }
  return history;
}

void hash_result(Sha256& hash, const std::string_view name, const pqxx::result& rows) {
  hash.add_length_prefixed(name);
  hash.add_length_prefixed(std::to_string(rows.size()));
  for (const auto& row : rows) {
    hash.add_length_prefixed("row");
    for (const auto& field : row) {
      hash.add_length_prefixed(field.is_null() ? "null" : "value");
      if (!field.is_null()) {
        hash.add_length_prefixed(field.as<std::string>());
      }
    }
  }
}

[[nodiscard]] std::string catalog_fingerprint(pqxx::transaction_base& transaction) {
  constexpr std::string_view schema_predicate =
      "n.nspname <> '_dbdiff' AND n.nspname <> 'information_schema' "
      "AND n.nspname !~ '^pg_'";
  Sha256 hash;
  hash.add_length_prefixed("dbdiff.postgresql.catalog-checkpoint.v1");
  hash_result(hash, "schemas",
              transaction.exec("SELECT n.nspname FROM pg_catalog.pg_namespace AS n WHERE " +
                               std::string{schema_predicate} +
                               " ORDER BY n.nspname COLLATE \"C\""));
  hash_result(
      hash, "relations",
      transaction.exec("SELECT n.nspname, c.relname, c.relkind::text, c.relpersistence::text, "
                       "       c.relrowsecurity, c.relforcerowsecurity, c.reloptions::text "
                       "FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " AND c.relkind IN ('r','p','v','m','S','f','i','I') "
                       "ORDER BY n.nspname COLLATE \"C\", c.relkind, c.relname COLLATE \"C\""));
  hash_result(hash, "columns",
              transaction.exec(
                  "SELECT n.nspname, c.relname, a.attnum, a.attname, "
                  "       a.atttypid::bigint, a.atttypmod, a.attnotnull, ad.adbin::text, "
                  "       a.attidentity::text, a.attgenerated::text "
                  "FROM pg_catalog.pg_attribute AS a "
                  "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                  "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                  "LEFT JOIN pg_catalog.pg_attrdef AS ad "
                  "  ON ad.adrelid = a.attrelid AND ad.adnum = a.attnum "
                  "WHERE " +
                  std::string{schema_predicate} +
                  " AND c.relkind IN ('r','p','v','m','f') AND a.attnum > 0 AND NOT a.attisdropped "
                  "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", a.attnum"));
  return hash.finish_hex();
}

[[nodiscard]] SchemaSnapshot
introspect_connection(pqxx::connection& connection,
                      const std::vector<std::string>& requested_schemas) {
  const auto managed_schemas = validate_managed_schemas(requested_schemas);
  SnapshotTransaction transaction{connection};
  execute_no_rows(transaction, "SET LOCAL search_path TO pg_catalog");

  const auto version_row =
      execute_one_row(transaction, "SELECT pg_catalog.current_setting('server_version_num')");
  SchemaSnapshot snapshot{
      .server_version = parse_server_version(version_row[0].as<std::string>()),
      .schemas = {},
      .tables = {},
  };

  if (managed_schemas.empty()) {
    transaction.commit();
    return snapshot;
  }

  const auto filter = schema_filter(transaction, managed_schemas);
  const auto schema_rows = transaction.exec("SELECT n.nspname "
                                            "FROM pg_catalog.pg_namespace AS n "
                                            "WHERE n.nspname IN (" +
                                            filter + ")");
  snapshot.schemas.reserve(static_cast<std::size_t>(schema_rows.size()));
  for (const auto& row : schema_rows) {
    snapshot.schemas.push_back(row[0].as<std::string>());
  }

  const auto table_rows =
      transaction.exec("SELECT n.nspname, c.relname, c.relpersistence::text, "
                       "       c.relrowsecurity, c.relforcerowsecurity "
                       "FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "  AND c.relkind = 'r' "
                       "  AND NOT c.relispartition "
                       "  AND c.relpersistence IN ('p', 'u') "
                       "  AND NOT EXISTS ("
                       "    SELECT 1 "
                       "    FROM pg_catalog.pg_depend AS d "
                       "    WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       "      AND d.objid = c.oid "
                       "      AND d.deptype = 'e')");

  std::map<QualifiedName, Table> tables;
  for (const auto& row : table_rows) {
    QualifiedName name{row[0].as<std::string>(), row[1].as<std::string>()};
    Table table{
        .name = name,
        .persistence = parse_persistence(row[2].as<std::string>()),
        .row_security = row[3].as<bool>(),
        .force_row_security = row[4].as<bool>(),
        .columns = {},
    };
    if (!tables.emplace(std::move(name), std::move(table)).second) {
      throw Error{ErrorCode::database, "PostgreSQL catalog returned a duplicate table"};
    }
  }

  const auto column_rows =
      transaction.exec("SELECT n.nspname, c.relname, a.attnum, a.attname, "
                       "       pg_catalog.format_type(a.atttypid, a.atttypmod), "
                       "       a.attnotnull, "
                       "       pg_catalog.pg_get_expr(ad.adbin, ad.adrelid, false), "
                       "       a.attidentity::text, a.attgenerated::text, "
                       "       CASE WHEN a.attcollation <> 0 AND a.attcollation <> t.typcollation "
                       "            THEN cn.nspname ELSE NULL END, "
                       "       CASE WHEN a.attcollation <> 0 AND a.attcollation <> t.typcollation "
                       "            THEN coll.collname ELSE NULL END "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid "
                       "LEFT JOIN pg_catalog.pg_attrdef AS ad "
                       "       ON ad.adrelid = a.attrelid AND ad.adnum = a.attnum "
                       "LEFT JOIN pg_catalog.pg_collation AS coll ON coll.oid = a.attcollation "
                       "LEFT JOIN pg_catalog.pg_namespace AS cn ON cn.oid = coll.collnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "  AND c.relkind = 'r' "
                       "  AND NOT c.relispartition "
                       "  AND c.relpersistence IN ('p', 'u') "
                       "  AND a.attnum > 0 "
                       "  AND NOT a.attisdropped "
                       "  AND NOT EXISTS ("
                       "    SELECT 1 "
                       "    FROM pg_catalog.pg_depend AS d "
                       "    WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       "      AND d.objid = c.oid "
                       "      AND d.deptype = 'e')");

  for (const auto& row : column_rows) {
    const QualifiedName table_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto table = tables.find(table_name);
    if (table == tables.end()) {
      throw Error{ErrorCode::database, "PostgreSQL catalog returned a column without its table"};
    }

    std::optional<std::string> default_expression;
    if (!row[6].is_null()) {
      default_expression = row[6].as<std::string>();
    }

    std::optional<QualifiedName> collation;
    if (!row[9].is_null() || !row[10].is_null()) {
      if (row[9].is_null() || row[10].is_null()) {
        throw Error{ErrorCode::database, "PostgreSQL returned an incomplete collation name"};
      }
      collation = QualifiedName{row[9].as<std::string>(), row[10].as<std::string>()};
    }

    table->second.columns.push_back(Column{
        .position = row[2].as<int>(),
        .name = row[3].as<std::string>(),
        .type = row[4].as<std::string>(),
        .not_null = row[5].as<bool>(),
        .default_expression = std::move(default_expression),
        .identity = parse_identity(row[7].as<std::string>()),
        .generated = parse_generated(row[8].as<std::string>()),
        .collation = std::move(collation),
    });
  }

  snapshot.tables.reserve(tables.size());
  for (auto& [name, table] : tables) {
    static_cast<void>(name);
    snapshot.tables.push_back(std::move(table));
  }
  transaction.commit();
  return normalize_snapshot(std::move(snapshot));
}

[[nodiscard]] std::uint64_t provisioning_owner(pqxx::transaction_base& transaction) {
  const auto row = execute_one_row(transaction, "SELECT r.oid::bigint "
                                                "FROM pg_catalog.pg_roles AS r "
                                                "WHERE r.rolname = pg_catalog.current_user");
  return row[0].as<std::uint64_t>();
}

[[nodiscard]] ServerVersion connection_server_version(pqxx::transaction_base& transaction) {
  const auto row =
      execute_one_row(transaction, "SELECT pg_catalog.current_setting('server_version_num')");
  return parse_server_version(row[0].as<std::string>());
}

void drop_scratch_database(const ConnectionLocator& provisioning_locator,
                           const ScratchIdentity& identity, const std::uint64_t owner_oid) {
  const auto connection_string = provisioning_locator.connection_string();
  pqxx::connection connection{connection_string.c_str()};
  pqxx::nontransaction transaction{connection};

  const auto rows =
      execute_one_parameter(transaction,
                            "SELECT d.datdba::bigint, "
                            "       pg_catalog.shobj_description(d.oid, 'pg_database') "
                            "FROM pg_catalog.pg_database AS d "
                            "WHERE d.datname = $1",
                            std::string{identity.name.value()});
  if (rows.empty()) {
    return;
  }
  if (rows.size() != 1) {
    throw Error{ErrorCode::database, "PostgreSQL returned duplicate scratch databases"};
  }

  const auto row = rows.front();
  if (row[0].as<std::uint64_t>() != owner_oid) {
    throw Error{ErrorCode::database, "refusing to remove a scratch database with another owner"};
  }

  if (row[1].is_null()) {
    throw Error{ErrorCode::database, "refusing to remove an unmarked scratch database"};
  }
  if (row[1].as<std::string>() != identity.marker.value()) {
    throw Error{ErrorCode::database, "refusing to remove a scratch database with another marker"};
  }

  execute_no_rows(transaction, "DROP DATABASE " + identity.name.quoted() + " WITH (FORCE)");
}

[[nodiscard]] bool try_creation_cleanup(const ConnectionLocator& provisioning_locator,
                                        const ScratchIdentity& identity,
                                        const std::uint64_t owner_oid) noexcept {
  try {
    drop_scratch_database(provisioning_locator, identity, owner_oid);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

[[noreturn]] void throw_connection_error(const std::string_view operation) {
  throw Error{ErrorCode::database, "PostgreSQL " + std::string{operation} + " failed"};
}

[[nodiscard]] bool source_forbidden_ddl(const std::string_view sql) {
  const auto words = statement_words(sql, 8U);
  if (words.size() < 2U) {
    return true;
  }
  if (words[0] == "alter" && words[1] == "system") {
    return true;
  }
  if ((words[0] == "create" || words[0] == "alter" || words[0] == "drop") &&
      (words[1] == "database" || words[1] == "tablespace" || words[1] == "role" ||
       words[1] == "user" || words[1] == "group" || words[1] == "subscription")) {
    return true;
  }
  return words[0] == "create" && (std::ranges::find(words, "temp") != words.end() ||
                                  std::ranges::find(words, "temporary") != words.end());
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

void hash_field(Sha256& hash, const std::string_view name, const std::string_view value) {
  hash.add_length_prefixed(name);
  hash.add_length_prefixed(value);
}

void hash_boolean(Sha256& hash, const std::string_view name, const bool value) {
  hash_field(hash, name, value ? "1" : "0");
}

void hash_optional(Sha256& hash, const std::string_view name,
                   const std::optional<std::string>& value) {
  hash_boolean(hash, "present", value.has_value());
  if (value.has_value()) {
    hash_field(hash, name, *value);
  }
}

[[nodiscard]] std::string semantic_hash_normalized(const SchemaSnapshot& snapshot) {
  Sha256 hash;
  hash.add_length_prefixed("dbdiff.postgresql.schema.v1");
  for (const auto& schema : snapshot.schemas) {
    hash_field(hash, "object", "schema");
    hash_field(hash, "name", schema);
  }
  for (const auto& table : snapshot.tables) {
    hash_field(hash, "object", "table");
    hash_field(hash, "schema", table.name.schema);
    hash_field(hash, "name", table.name.name);
    hash_field(hash, "persistence", std::to_string(static_cast<int>(table.persistence)));
    hash_boolean(hash, "row_security", table.row_security);
    hash_boolean(hash, "force_row_security", table.force_row_security);
    for (const auto& column : table.columns) {
      hash_field(hash, "child", "column");
      hash_field(hash, "position", std::to_string(column.position));
      hash_field(hash, "name", column.name);
      hash_field(hash, "type", column.type);
      hash_boolean(hash, "not_null", column.not_null);
      hash_optional(hash, "default", column.default_expression);
      hash_field(hash, "identity", std::to_string(static_cast<int>(column.identity)));
      hash_field(hash, "generated", std::to_string(static_cast<int>(column.generated)));
      hash_boolean(hash, "collation_present", column.collation.has_value());
      if (column.collation.has_value()) {
        hash_field(hash, "collation_schema", column.collation->schema);
        hash_field(hash, "collation_name", column.collation->name);
      }
    }
  }
  return hash.finish_hex();
}

} // namespace

BackendKind kind() noexcept { return BackendKind::postgresql; }

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

ParsedScript parse_migration(std::string sql) {
  auto statements = scan_statements(sql);
  validate_migration_statements(sql, statements);
  return build_execution_units(std::move(sql), std::move(statements));
}

MigrationApplyResult validate_migration_resume(const std::optional<MigrationHistoryEntry>& history,
                                               const ParsedScript& candidate,
                                               const std::string_view exact_file_sha256,
                                               const bool resume) {
  require_file_sha256(exact_file_sha256);
  if (sha256_hex(candidate.sql) != exact_file_sha256) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration SQL does not match its exact file hash"};
  }
  if (!history.has_value()) {
    if (resume) {
      throw Error{ErrorCode::migration,
                  "cannot resume a PostgreSQL migration version with no stored attempt"};
    }
    return {};
  }

  std::vector<std::string> completed_hashes;
  completed_hashes.reserve(history->units.size());
  for (std::size_t index = 0U; index < history->units.size(); ++index) {
    const auto& unit = history->units[index];
    if (unit.ordinal != index) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history units are not a contiguous prefix"};
    }
    validate_stored_hash(unit.exact_sha256, "unit hash");
    if (!unit.before_schema_sha256.empty()) {
      validate_stored_hash(unit.before_schema_sha256, "unit before-schema hash");
    }
    if (!unit.after_schema_sha256.empty()) {
      validate_stored_hash(unit.after_schema_sha256, "unit after-schema hash");
    }
    if (unit.state == MigrationUnitState::started) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration has an uncertain standalone unit at ordinal " +
                      std::to_string(unit.ordinal) +
                      "; recover the stored revision and reconcile it manually"};
    }
    if (index >= candidate.units.size() ||
        unit.explicit_transaction != candidate.units[index].explicit_transaction) {
      throw Error{ErrorCode::migration,
                  "edited PostgreSQL migration changes a completed unit boundary"};
    }
    completed_hashes.push_back(unit.exact_sha256);
  }
  validate_completed_prefix(completed_hashes, candidate);

  if (history->completed_file_sha256.has_value()) {
    if (*history->completed_file_sha256 != exact_file_sha256) {
      throw Error{ErrorCode::drift,
                  "completed PostgreSQL migration file bytes no longer match history"};
    }
    if (completed_hashes.size() != candidate.units.size()) {
      throw Error{ErrorCode::drift,
                  "completed PostgreSQL migration history has an incomplete unit prefix"};
    }
    return MigrationApplyResult{
        .already_completed = true,
        .completed_unit_count = completed_hashes.size(),
        .completed_file_sha256 = std::string{exact_file_sha256},
    };
  }
  if (!resume) {
    throw Error{ErrorCode::migration,
                "PostgreSQL migration has an incomplete attempt; retry with resume enabled"};
  }
  if (!candidate.units.empty() && completed_hashes.size() == candidate.units.size()) {
    throw Error{ErrorCode::drift,
                "PostgreSQL migration completed every unit without an atomic file checkpoint"};
  }
  return MigrationApplyResult{
      .already_completed = false,
      .completed_unit_count = completed_hashes.size(),
      .completed_file_sha256 = {},
  };
}

ConnectionLocator::ConnectionLocator(std::vector<Option> options) : options_{std::move(options)} {}

ConnectionLocator ConnectionLocator::parse(const std::string_view locator) {
  require_no_nul(locator, "PostgreSQL connection locator");
  const std::string locator_string{locator};
  char* error_message = nullptr;
  std::unique_ptr<PQconninfoOption, ConninfoDeleter> parsed{
      PQconninfoParse(locator_string.c_str(), &error_message)};
  const std::unique_ptr<char, LibpqMemoryDeleter> error{error_message};
  if (!parsed) {
    throw Error{ErrorCode::configuration, "invalid PostgreSQL connection locator"};
  }

  std::vector<Option> options;
  for (auto* option = parsed.get(); option->keyword != nullptr; ++option) {
    if (option->val == nullptr) {
      continue;
    }
    const bool secret = (option->dispchar != nullptr && std::strcmp(option->dispchar, "*") == 0) ||
                        std::string_view{option->keyword} == "password" ||
                        std::string_view{option->keyword} == "sslpassword" ||
                        std::string_view{option->keyword} == "oauth_client_secret";
    options.push_back(Option{option->keyword, option->val, secret});
  }
  return ConnectionLocator{std::move(options)};
}

ConnectionLocator ConnectionLocator::with_database(const std::string_view database) const {
  require_no_nul(database, "PostgreSQL database name");
  if (database.empty()) {
    throw Error{ErrorCode::configuration, "PostgreSQL database name must not be empty"};
  }

  auto options = options_;
  const auto existing = std::ranges::find(options, std::string_view{"dbname"}, &Option::keyword);
  if (existing == options.end()) {
    options.push_back(Option{"dbname", std::string{database}, false});
  } else {
    existing->value = database;
  }
  return ConnectionLocator{std::move(options)};
}

std::optional<std::string_view>
ConnectionLocator::value(const std::string_view keyword) const noexcept {
  const auto option = std::ranges::find(options_, keyword, &Option::keyword);
  if (option == options_.end()) {
    return std::nullopt;
  }
  return option->value;
}

std::string ConnectionLocator::connection_string() const {
  std::string result;
  for (const auto& option : options_) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += option.keyword;
    result.push_back('=');
    result += quote_conninfo_value(option.value);
  }
  return result;
}

std::string ConnectionLocator::redacted() const {
  std::string result;
  for (const auto& option : options_) {
    if (!result.empty()) {
      result.push_back(' ');
    }
    result += option.keyword;
    result.push_back('=');
    result += quote_conninfo_value(option.secret ? std::string_view{"<redacted>"}
                                                 : std::string_view{option.value});
  }
  return result;
}

bool ConnectionLocator::empty() const noexcept { return options_.empty(); }

ServerVersion validate_server_version(const int version_number) {
  if (version_number < minimum_server_version || version_number >= maximum_server_version) {
    throw Error{ErrorCode::unsupported, "dbdiff supports PostgreSQL versions 15 through 18"};
  }
  return ServerVersion{
      .number = version_number,
      .major = version_number / 10000,
      .patch = version_number % 10000,
  };
}

ServerVersion parse_server_version(const std::string_view version_number) {
  int parsed{};
  const auto [end, error] =
      std::from_chars(version_number.data(), version_number.data() + version_number.size(), parsed);
  if (error != std::errc{} || end != version_number.data() + version_number.size()) {
    throw Error{ErrorCode::database, "PostgreSQL returned an invalid server version"};
  }
  return validate_server_version(parsed);
}

std::string quote_identifier(const std::string_view identifier) {
  require_no_nul(identifier, "PostgreSQL identifier");
  if (identifier.empty()) {
    throw Error{ErrorCode::configuration, "PostgreSQL identifier must not be empty"};
  }

  std::string result;
  result.reserve(identifier.size() + 2);
  result.push_back('"');
  for (const char character : identifier) {
    if (character == '"') {
      result.push_back('"');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

std::string quote_literal(const std::string_view value) {
  require_no_nul(value, "PostgreSQL literal");

  std::string result{"E'"};
  result.reserve(value.size() + 3);
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (byte) {
    case '\'':
      result += "\\'";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    default:
      if (byte < 0x20U || byte == 0x7fU) {
        result.push_back('\\');
        result.push_back(static_cast<char>('0' + ((byte >> 6U) & 0x07U)));
        result.push_back(static_cast<char>('0' + ((byte >> 3U) & 0x07U)));
        result.push_back(static_cast<char>('0' + (byte & 0x07U)));
      } else {
        result.push_back(character);
      }
      break;
    }
  }
  result.push_back('\'');
  return result;
}

ScratchName::ScratchName(std::string value, std::string token)
    : value_{std::move(value)}, token_{std::move(token)} {}

ScratchName ScratchName::from_token(const std::string_view token) {
  if (!is_lower_hex_token(token)) {
    throw Error{ErrorCode::configuration,
                "PostgreSQL scratch token must contain 32 lowercase hexadecimal characters"};
  }
  return ScratchName{std::string{scratch_prefix} + std::string{token}, std::string{token}};
}

ScratchName ScratchName::generate() { return from_token(random_token()); }

std::string_view ScratchName::value() const noexcept { return value_; }

std::string_view ScratchName::token() const noexcept { return token_; }

std::string ScratchName::quoted() const { return quote_identifier(value_); }

ScratchMarker::ScratchMarker(std::string value) : value_{std::move(value)} {}

ScratchMarker ScratchMarker::create(const std::string_view token,
                                    const std::int64_t creation_epoch_seconds) {
  if (!is_lower_hex_token(token)) {
    throw Error{ErrorCode::configuration,
                "PostgreSQL scratch token must contain 32 lowercase hexadecimal characters"};
  }
  if (creation_epoch_seconds < 0) {
    throw Error{ErrorCode::configuration, "PostgreSQL scratch creation time must not be negative"};
  }
  return ScratchMarker{"dbdiff:scratch:v1:" + std::string{token} + ":" +
                       std::to_string(creation_epoch_seconds)};
}

std::string_view ScratchMarker::value() const noexcept { return value_; }

std::string ScratchMarker::quoted() const { return quote_literal(value_); }

ScratchIdentity ScratchIdentity::generate() {
  const auto token = random_token();
  return from_token(token, current_epoch_seconds());
}

ScratchIdentity ScratchIdentity::from_token(const std::string_view token,
                                            const std::int64_t creation_epoch_seconds) {
  return ScratchIdentity{
      .name = ScratchName::from_token(token),
      .marker = ScratchMarker::create(token, creation_epoch_seconds),
  };
}

SchemaSnapshot normalize_snapshot(SchemaSnapshot snapshot) {
  snapshot.server_version = validate_server_version(snapshot.server_version.number);
  std::ranges::sort(snapshot.schemas);
  if (std::ranges::adjacent_find(snapshot.schemas) != snapshot.schemas.end()) {
    throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains duplicate schemas"};
  }
  for (const auto& schema : snapshot.schemas) {
    require_no_nul(schema, "PostgreSQL schema name");
    if (schema.empty() || schema == metadata_schema) {
      throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains an invalid schema"};
    }
  }

  std::ranges::sort(snapshot.tables, {}, &Table::name);
  if (std::ranges::adjacent_find(snapshot.tables, {}, &Table::name) != snapshot.tables.end()) {
    throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains duplicate tables"};
  }

  for (auto& table : snapshot.tables) {
    if (!std::ranges::binary_search(snapshot.schemas, table.name.schema)) {
      throw Error{ErrorCode::database, "PostgreSQL table belongs to an unknown schema"};
    }
    require_no_nul(table.name.name, "PostgreSQL table name");
    if (table.name.name.empty()) {
      throw Error{ErrorCode::database, "PostgreSQL table name must not be empty"};
    }

    std::ranges::sort(table.columns, [](const Column& left, const Column& right) {
      if (left.position != right.position) {
        return left.position < right.position;
      }
      return left.name < right.name;
    });

    std::set<std::string> column_names;
    int previous_position = 0;
    int canonical_position = 0;
    for (auto& column : table.columns) {
      require_no_nul(column.name, "PostgreSQL column name");
      require_no_nul(column.type, "PostgreSQL column type");
      if (column.default_expression.has_value()) {
        require_no_nul(*column.default_expression, "PostgreSQL column expression");
      }
      if (column.collation.has_value()) {
        require_no_nul(column.collation->schema, "PostgreSQL collation schema");
        require_no_nul(column.collation->name, "PostgreSQL collation name");
        if (column.collation->schema.empty() || column.collation->name.empty()) {
          throw Error{ErrorCode::database,
                      "PostgreSQL schema snapshot contains an invalid collation"};
        }
      }
      if (column.position <= 0 || column.position == previous_position || column.name.empty() ||
          column.type.empty() || !column_names.insert(column.name).second) {
        throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains an invalid column"};
      }
      if (column.identity != IdentityGeneration::none &&
          column.generated != GeneratedStorage::none) {
        throw Error{ErrorCode::database, "PostgreSQL column cannot be both identity and generated"};
      }
      if (column.identity != IdentityGeneration::none && column.default_expression.has_value()) {
        throw Error{ErrorCode::database,
                    "PostgreSQL identity column cannot have a separate default expression"};
      }
      if (column.generated != GeneratedStorage::none && !column.default_expression.has_value()) {
        throw Error{ErrorCode::database, "PostgreSQL generated column is missing its expression"};
      }
      if (column.generated == GeneratedStorage::virtual_column &&
          snapshot.server_version.major < 18) {
        throw Error{ErrorCode::unsupported,
                    "virtual generated columns require PostgreSQL 18 or newer"};
      }
      previous_position = column.position;
      column.position = ++canonical_position;
    }
  }
  snapshot.semantic_hash = semantic_hash_normalized(snapshot);
  return snapshot;
}

std::string semantic_hash(const SchemaSnapshot& snapshot) {
  return normalize_snapshot(snapshot).semantic_hash;
}

namespace {

[[nodiscard]] std::string qualified_sql(const QualifiedName& name) {
  return quote_identifier(name.schema) + "." + quote_identifier(name.name);
}

[[nodiscard]] std::string qualified_identity(const QualifiedName& name) {
  return name.schema + "." + name.name;
}

[[nodiscard]] std::string column_identity(const QualifiedName& table,
                                          const std::string_view column) {
  return qualified_identity(table) + "." + std::string{column};
}

[[nodiscard]] std::string operation_id(const std::string_view action,
                                       const std::string_view identity) {
  std::string input{action};
  input.push_back('\0');
  input.append(identity);
  return "postgresql." + std::string{action} + "." + sha256_hex(input);
}

[[nodiscard]] std::string render_column_definition(const Column& column) {
  std::string sql = quote_identifier(column.name) + " " + column.type;
  if (column.collation.has_value()) {
    sql += " COLLATE " + qualified_sql(*column.collation);
  }
  if (column.generated != GeneratedStorage::none) {
    sql += " GENERATED ALWAYS AS (" + column.default_expression.value_or(std::string{}) + ") ";
    sql += column.generated == GeneratedStorage::stored ? "STORED" : "VIRTUAL";
  } else if (column.identity != IdentityGeneration::none) {
    sql += column.identity == IdentityGeneration::always ? " GENERATED ALWAYS AS IDENTITY"
                                                         : " GENERATED BY DEFAULT AS IDENTITY";
  } else if (column.default_expression.has_value()) {
    sql += " DEFAULT " + *column.default_expression;
  }
  if (column.not_null) {
    sql += " NOT NULL";
  }
  return sql;
}

[[nodiscard]] std::string render_create_table(const Table& table) {
  std::ostringstream sql;
  sql << "CREATE ";
  if (table.persistence == TablePersistence::unlogged) {
    sql << "UNLOGGED ";
  }
  sql << "TABLE " << qualified_sql(table.name) << " (";
  for (std::size_t index = 0U; index < table.columns.size(); ++index) {
    if (index != 0U) {
      sql << ", ";
    }
    sql << render_column_definition(table.columns[index]);
  }
  sql << ");";
  return sql.str();
}

[[nodiscard]] bool same_column_shape(const Column& left, const Column& right) {
  return left.type == right.type && left.not_null == right.not_null &&
         left.default_expression == right.default_expression && left.identity == right.identity &&
         left.generated == right.generated && left.collation == right.collation;
}

[[nodiscard]] bool same_table_shape(const Table& left, const Table& right) {
  if (left.persistence != right.persistence || left.row_security != right.row_security ||
      left.force_row_security != right.force_row_security ||
      left.columns.size() != right.columns.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.columns.size(); ++index) {
    if (left.columns[index] != right.columns[index]) {
      return false;
    }
  }
  return true;
}

void add_diagnostic(MigrationPlan& migration_plan, std::string diagnostic) {
  migration_plan.draft = true;
  if (std::ranges::find(migration_plan.diagnostics, diagnostic) ==
      migration_plan.diagnostics.end()) {
    migration_plan.diagnostics.push_back(std::move(diagnostic));
  }
}

[[nodiscard]] std::string add_operation(MigrationPlan& migration_plan,
                                        const std::string_view action,
                                        const std::string_view identity, std::string sql,
                                        HazardSet hazards = {},
                                        std::vector<std::string> dependencies = {}) {
  const auto id = operation_id(action, identity);
  migration_plan.operations.push_back(Operation{
      .id = id,
      .object_identity = std::string{identity},
      .dependencies = std::move(dependencies),
      .transaction_mode = TransactionMode::required,
      .sql = std::move(sql),
      .hazards = std::move(hazards),
      .expected_before = {},
      .expected_after = {},
  });
  return id;
}

[[nodiscard]] std::map<QualifiedName, const Table*> table_index(const std::vector<Table>& tables) {
  std::map<QualifiedName, const Table*> result;
  for (const auto& table : tables) {
    result.emplace(table.name, &table);
  }
  return result;
}

[[nodiscard]] std::map<std::string, const Column*>
column_index(const std::vector<Column>& columns) {
  std::map<std::string, const Column*> result;
  for (const auto& column : columns) {
    result.emplace(column.name, &column);
  }
  return result;
}

void plan_row_security(MigrationPlan& migration_plan, const Table& from, const Table& to,
                       std::vector<std::string> dependencies = {}) {
  const auto table = qualified_sql(to.name);
  const auto identity = qualified_identity(to.name);
  if (from.row_security != to.row_security) {
    const auto action =
        to.row_security ? "table.row_security.enable" : "table.row_security.disable";
    const auto sql =
        "ALTER TABLE " + table +
        (to.row_security ? " ENABLE ROW LEVEL SECURITY;" : " DISABLE ROW LEVEL SECURITY;");
    dependencies = {add_operation(migration_plan, action, identity, sql,
                                  HazardSet{Hazard::write_lock}, std::move(dependencies))};
  }
  if (from.force_row_security != to.force_row_security) {
    const auto action =
        to.force_row_security ? "table.row_security.force" : "table.row_security.no_force";
    const auto sql =
        "ALTER TABLE " + table +
        (to.force_row_security ? " FORCE ROW LEVEL SECURITY;" : " NO FORCE ROW LEVEL SECURITY;");
    static_cast<void>(add_operation(migration_plan, action, identity, sql,
                                    HazardSet{Hazard::write_lock}, std::move(dependencies)));
  }
}

} // namespace

MigrationPlan plan(SchemaSnapshot from, SchemaSnapshot to) {
  from = normalize_snapshot(std::move(from));
  to = normalize_snapshot(std::move(to));
  if (from.server_version.major != to.server_version.major) {
    throw Error{ErrorCode::unsupported,
                "PostgreSQL planning requires snapshots from the same server major version"};
  }

  MigrationPlan migration_plan;
  const std::set<std::string, std::less<>> from_schemas(from.schemas.begin(), from.schemas.end());
  const std::set<std::string, std::less<>> to_schemas(to.schemas.begin(), to.schemas.end());
  const auto from_tables = table_index(from.tables);
  const auto to_tables = table_index(to.tables);

  std::map<std::string, std::string, std::less<>> schema_create_operations;
  for (const auto& schema : to_schemas) {
    if (!from_schemas.contains(schema)) {
      schema_create_operations.emplace(
          schema, add_operation(migration_plan, "schema.create", schema,
                                "CREATE SCHEMA " + quote_identifier(schema) + ";"));
    }
  }

  std::set<QualifiedName> possible_table_renames_from;
  std::set<QualifiedName> possible_table_renames_to;
  for (const auto& [from_name, from_table] : from_tables) {
    if (to_tables.contains(from_name)) {
      continue;
    }
    for (const auto& [to_name, to_table] : to_tables) {
      if (!from_tables.contains(to_name) && same_table_shape(*from_table, *to_table)) {
        possible_table_renames_from.insert(from_name);
        possible_table_renames_to.insert(to_name);
        add_diagnostic(migration_plan, "possible table rename from " + qualified_sql(from_name) +
                                           " to " + qualified_sql(to_name) +
                                           "; write an explicit ALTER TABLE ... RENAME migration");
      }
    }
  }

  std::map<std::string, std::vector<std::string>, std::less<>> dropped_table_operations;
  for (const auto& [name, table] : to_tables) {
    if (from_tables.contains(name) || possible_table_renames_to.contains(name)) {
      continue;
    }
    std::vector<std::string> dependencies;
    if (const auto schema = schema_create_operations.find(name.schema);
        schema != schema_create_operations.end()) {
      dependencies.push_back(schema->second);
    }
    const auto create_id = add_operation(migration_plan, "table.create", qualified_identity(name),
                                         render_create_table(*table), {}, std::move(dependencies));
    const Table empty_table{
        .name = name,
        .persistence = table->persistence,
        .row_security = false,
        .force_row_security = false,
        .columns = table->columns,
    };
    plan_row_security(migration_plan, empty_table, *table, {create_id});
  }

  for (const auto& [name, from_table] : from_tables) {
    const auto target = to_tables.find(name);
    if (target == to_tables.end()) {
      if (!possible_table_renames_from.contains(name)) {
        const auto id = add_operation(migration_plan, "table.drop", qualified_identity(name),
                                      "DROP TABLE " + qualified_sql(name) + " RESTRICT;",
                                      HazardSet{Hazard::data_loss, Hazard::write_lock});
        dropped_table_operations[name.schema].push_back(id);
      }
      continue;
    }

    const auto* to_table = target->second;
    std::vector<std::string> table_dependencies;
    if (from_table->persistence != to_table->persistence) {
      const auto sql =
          "ALTER TABLE " + qualified_sql(name) +
          (to_table->persistence == TablePersistence::unlogged ? " SET UNLOGGED;" : " SET LOGGED;");
      table_dependencies = {add_operation(migration_plan, "table.persistence",
                                          qualified_identity(name), sql,
                                          HazardSet{Hazard::table_rewrite, Hazard::write_lock})};
    }

    const auto from_columns = column_index(from_table->columns);
    const auto to_columns = column_index(to_table->columns);
    std::set<std::string, std::less<>> possible_column_renames_from;
    std::set<std::string, std::less<>> possible_column_renames_to;
    for (const auto& [from_name, from_column] : from_columns) {
      if (to_columns.contains(from_name)) {
        continue;
      }
      for (const auto& [to_name, to_column] : to_columns) {
        if (!from_columns.contains(to_name) && same_column_shape(*from_column, *to_column)) {
          possible_column_renames_from.insert(from_name);
          possible_column_renames_to.insert(to_name);
          add_diagnostic(migration_plan,
                         "possible column rename from " + quote_identifier(from_name) + " to " +
                             quote_identifier(to_name) + " on " + qualified_sql(name) +
                             "; write an explicit RENAME COLUMN migration");
        }
      }
    }

    std::vector<std::string> from_common_order;
    std::vector<std::string> to_common_order;
    for (const auto& column : from_table->columns) {
      if (to_columns.contains(column.name)) {
        from_common_order.push_back(column.name);
      }
    }
    bool saw_added_column = false;
    std::set<std::string, std::less<>> inserted_columns;
    for (const auto& column : to_table->columns) {
      if (from_columns.contains(column.name)) {
        to_common_order.push_back(column.name);
        if (saw_added_column) {
          for (const auto& candidate : to_table->columns) {
            if (!from_columns.contains(candidate.name) && candidate.position < column.position) {
              inserted_columns.insert(candidate.name);
            }
          }
        }
      } else {
        saw_added_column = true;
      }
    }
    if (from_common_order != to_common_order) {
      add_diagnostic(migration_plan, "column reordering on " + qualified_sql(name) +
                                         " cannot be represented safely by PostgreSQL ALTER TABLE");
    }
    for (const auto& inserted : inserted_columns) {
      add_diagnostic(migration_plan,
                     "column " + quote_identifier(inserted) + " is inserted into the middle of " +
                         qualified_sql(name) +
                         "; PostgreSQL ADD COLUMN can only append it without rebuilding the table");
    }

    for (const auto& column : from_table->columns) {
      if (!to_columns.contains(column.name) &&
          !possible_column_renames_from.contains(column.name)) {
        table_dependencies = {
            add_operation(migration_plan, "column.drop", column_identity(name, column.name),
                          "ALTER TABLE " + qualified_sql(name) + " DROP COLUMN " +
                              quote_identifier(column.name) + " RESTRICT;",
                          HazardSet{Hazard::data_loss, Hazard::write_lock}, table_dependencies)};
      }
    }

    for (const auto& column : to_table->columns) {
      if (from_columns.contains(column.name) || possible_column_renames_to.contains(column.name) ||
          inserted_columns.contains(column.name)) {
        continue;
      }
      if (column.not_null && !column.default_expression.has_value() &&
          column.identity == IdentityGeneration::none &&
          column.generated == GeneratedStorage::none) {
        add_diagnostic(migration_plan, "adding NOT NULL column " + quote_identifier(column.name) +
                                           " to " + qualified_sql(name) +
                                           " requires an explicit backfill/default migration");
        continue;
      }
      HazardSet hazards{Hazard::write_lock};
      if (column.default_expression.has_value() || column.generated != GeneratedStorage::none) {
        hazards.insert(Hazard::table_rewrite);
      }
      table_dependencies = {add_operation(migration_plan, "column.add",
                                          column_identity(name, column.name),
                                          "ALTER TABLE " + qualified_sql(name) + " ADD COLUMN " +
                                              render_column_definition(column) + ";",
                                          std::move(hazards), table_dependencies)};
    }

    for (const auto& from_column : from_table->columns) {
      const auto target_column = to_columns.find(from_column.name);
      if (target_column == to_columns.end()) {
        continue;
      }
      const auto& to_column = *target_column->second;
      const bool generated_expression_changed =
          from_column.generated != GeneratedStorage::none &&
          from_column.default_expression != to_column.default_expression;
      if (from_column.type != to_column.type || from_column.identity != to_column.identity ||
          from_column.generated != to_column.generated ||
          from_column.collation != to_column.collation || generated_expression_changed) {
        add_diagnostic(migration_plan,
                       "column " + quote_identifier(from_column.name) + " on " +
                           qualified_sql(name) +
                           " changes type, collation, identity, or generation semantics; write an "
                           "explicit ALTER COLUMN migration with any required USING expression");
        continue;
      }

      if (from_column.generated == GeneratedStorage::none &&
          from_column.identity == IdentityGeneration::none &&
          from_column.default_expression != to_column.default_expression) {
        const auto sql = to_column.default_expression.has_value()
                             ? "ALTER TABLE " + qualified_sql(name) + " ALTER COLUMN " +
                                   quote_identifier(from_column.name) + " SET DEFAULT " +
                                   *to_column.default_expression + ";"
                             : "ALTER TABLE " + qualified_sql(name) + " ALTER COLUMN " +
                                   quote_identifier(from_column.name) + " DROP DEFAULT;";
        table_dependencies = {add_operation(
            migration_plan,
            to_column.default_expression.has_value() ? "column.default.set" : "column.default.drop",
            column_identity(name, from_column.name), sql, HazardSet{Hazard::write_lock},
            table_dependencies)};
      }
      if (from_column.not_null != to_column.not_null) {
        HazardSet hazards{Hazard::write_lock};
        if (to_column.not_null) {
          hazards.insert(Hazard::constraint_scan);
        }
        const auto sql = "ALTER TABLE " + qualified_sql(name) + " ALTER COLUMN " +
                         quote_identifier(from_column.name) +
                         (to_column.not_null ? " SET NOT NULL;" : " DROP NOT NULL;");
        table_dependencies = {add_operation(
            migration_plan, to_column.not_null ? "column.not_null.set" : "column.not_null.drop",
            column_identity(name, from_column.name), sql, std::move(hazards), table_dependencies)};
      }
    }
    plan_row_security(migration_plan, *from_table, *to_table, table_dependencies);
  }

  for (const auto& schema : from_schemas) {
    if (to_schemas.contains(schema)) {
      continue;
    }
    const bool blocked_table =
        std::ranges::any_of(possible_table_renames_from,
                            [&schema](const QualifiedName& name) { return name.schema == schema; });
    if (blocked_table) {
      add_diagnostic(migration_plan,
                     "schema " + quote_identifier(schema) +
                         " cannot be dropped while a possible table rename is unresolved");
      continue;
    }
    auto dependencies = dropped_table_operations[schema];
    static_cast<void>(add_operation(migration_plan, "schema.drop", schema,
                                    "DROP SCHEMA " + quote_identifier(schema) + " RESTRICT;", {},
                                    std::move(dependencies)));
  }

  return migration_plan;
}

std::string render_plan(const MigrationPlan& migration_plan) {
  std::ostringstream sql;
  if (migration_plan.draft) {
    sql << "-- draft: manual edits required\n";
    for (const auto& diagnostic : migration_plan.diagnostics) {
      std::string single_line = diagnostic;
      std::ranges::replace(single_line, '\n', ' ');
      sql << "-- TODO: " << single_line << '\n';
    }
  }
  if (migration_plan.operations.empty()) {
    return sql.str();
  }

  const auto order = deterministic_operation_order(migration_plan.operations);
  for (const auto index : order) {
    if (migration_plan.operations[index].transaction_mode == TransactionMode::forbidden) {
      throw Error{ErrorCode::unsupported,
                  "PostgreSQL focused planner cannot render nontransactional operations"};
    }
  }
  sql << "BEGIN;\n";
  for (const auto index : order) {
    const auto& operation = migration_plan.operations[index];
    if (!operation.hazards.empty()) {
      sql << "-- hazards:";
      for (const auto hazard : operation.hazards) {
        sql << ' ' << hazard_name(hazard);
      }
      sql << '\n';
    }
    sql << operation.sql;
    if (!operation.sql.ends_with('\n')) {
      sql << '\n';
    }
  }
  sql << "COMMIT;\n";
  return sql.str();
}

SchemaSnapshot introspect_database(const ConnectionLocator& locator,
                                   const std::vector<std::string>& managed_schemas) {
  try {
    const auto connection_string = locator.connection_string();
    pqxx::connection connection{connection_string.c_str()};
    return introspect_connection(connection, managed_schemas);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw_connection_error("schema introspection");
  }
}

namespace {

[[nodiscard]] std::optional<MigrationHistoryEntry> history_entry(const MigrationHistory& history,
                                                                 const std::string_view version) {
  const auto found = std::ranges::find(history.entries, version, &MigrationHistoryEntry::version);
  if (found == history.entries.end()) {
    return std::nullopt;
  }
  return *found;
}

void validate_history_for_apply(const MigrationHistory& history, const std::string_view version,
                                const bool version_exists) {
  for (const auto& entry : history.entries) {
    if (entry.backend != "postgresql") {
      throw Error{ErrorCode::drift,
                  "PostgreSQL migration history contains a record for another backend"};
    }
    if (!entry.completed_file_sha256.has_value() && entry.version != version) {
      throw Error{ErrorCode::migration,
                  "another PostgreSQL migration version has an incomplete attempt: " +
                      entry.version};
    }
    if (!version_exists && entry.version > version) {
      throw Error{ErrorCode::migration,
                  "PostgreSQL migration versions must be applied in lexical order"};
    }
  }
}

[[nodiscard]] std::size_t record_revision(pqxx::transaction_base& transaction,
                                          const std::string_view version,
                                          const std::string_view engine_version,
                                          const std::string_view exact_file_sha256,
                                          const std::string_view sql) {
  const auto rows =
      execute_parameters(transaction,
                         "WITH migration AS ("
                         "  INSERT INTO \"_dbdiff\".\"migrations\" "
                         "    (version, backend, engine_version, attempted_file_sha256) "
                         "  VALUES ($1, 'postgresql', $2, $3) "
                         "  ON CONFLICT (version) DO UPDATE "
                         "  SET engine_version = EXCLUDED.engine_version, "
                         "      attempted_file_sha256 = EXCLUDED.attempted_file_sha256, "
                         "      updated_at = pg_catalog.clock_timestamp() "
                         "  WHERE \"_dbdiff\".\"migrations\".completed_file_sha256 IS NULL "
                         "  RETURNING version"
                         "), existing AS ("
                         "  SELECT ordinal FROM \"_dbdiff\".\"revisions\" "
                         "  WHERE version = $1 AND exact_file_sha256 = $3 "
                         "    AND EXISTS (SELECT 1 FROM migration)"
                         "), next_ordinal AS ("
                         "  SELECT COALESCE(MAX(ordinal) + 1, 0)::bigint AS ordinal "
                         "  FROM \"_dbdiff\".\"revisions\" WHERE version = $1"
                         "), inserted AS ("
                         "  INSERT INTO \"_dbdiff\".\"revisions\" "
                         "    (version, ordinal, exact_file_sha256, sql) "
                         "  SELECT $1, next_ordinal.ordinal, $3, $4 FROM next_ordinal "
                         "  WHERE NOT EXISTS (SELECT 1 FROM existing) "
                         "    AND EXISTS (SELECT 1 FROM migration) RETURNING ordinal"
                         ") SELECT ordinal FROM existing UNION ALL SELECT ordinal FROM inserted",
                         version, engine_version, exact_file_sha256, sql);
  if (rows.size() != 1U) {
    throw Error{ErrorCode::database,
                "PostgreSQL could not atomically record the exact migration revision"};
  }
  return history_ordinal(rows.front()[0], "migration revision ordinal");
}

void checkpoint_completed_unit(pqxx::transaction_base& transaction, const std::string_view version,
                               const ExecutionUnit& unit,
                               const std::string_view before_schema_sha256,
                               const std::string_view after_schema_sha256,
                               const std::size_t revision_ordinal, const bool final_unit,
                               const std::string_view exact_file_sha256) {
  if (final_unit) {
    const auto rows = execute_parameters(
        transaction,
        "WITH inserted AS ("
        "  INSERT INTO \"_dbdiff\".\"units\" "
        "    (version, ordinal, exact_sha256, explicit_transaction, "
        "     before_schema_sha256, after_schema_sha256, state, revision_ordinal) "
        "  VALUES ($1, $2, $3, $4, $5, $6, 'completed', $7) RETURNING 1"
        ") "
        "UPDATE \"_dbdiff\".\"migrations\" "
        "SET completed_file_sha256 = $8, attempted_file_sha256 = $8, "
        "    updated_at = pg_catalog.clock_timestamp() "
        "WHERE version = $1 AND EXISTS (SELECT 1 FROM inserted) RETURNING version",
        version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256,
        unit.explicit_transaction, before_schema_sha256, after_schema_sha256,
        static_cast<std::uint64_t>(revision_ordinal), exact_file_sha256);
    if (rows.size() != 1U) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL could not atomically checkpoint the final migration unit"};
    }
    return;
  }
  static_cast<void>(
      execute_parameters(transaction,
                         "INSERT INTO \"_dbdiff\".\"units\" "
                         "  (version, ordinal, exact_sha256, explicit_transaction, "
                         "   before_schema_sha256, after_schema_sha256, state, revision_ordinal) "
                         "VALUES ($1, $2, $3, $4, $5, $6, 'completed', $7)",
                         version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256,
                         unit.explicit_transaction, before_schema_sha256, after_schema_sha256,
                         static_cast<std::uint64_t>(revision_ordinal)));
}

void checkpoint_started_unit(pqxx::transaction_base& transaction, const std::string_view version,
                             const ExecutionUnit& unit, const std::string_view before_schema_sha256,
                             const std::size_t revision_ordinal) {
  static_cast<void>(
      execute_parameters(transaction,
                         "INSERT INTO \"_dbdiff\".\"units\" "
                         "  (version, ordinal, exact_sha256, explicit_transaction, "
                         "   before_schema_sha256, after_schema_sha256, state, revision_ordinal) "
                         "VALUES ($1, $2, $3, false, $4, '', 'started', $5)",
                         version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256,
                         before_schema_sha256, static_cast<std::uint64_t>(revision_ordinal)));
}

void remove_failed_started_unit(pqxx::transaction_base& transaction, const std::string_view version,
                                const ExecutionUnit& unit) {
  static_cast<void>(execute_parameters(
      transaction,
      "DELETE FROM \"_dbdiff\".\"units\" "
      "WHERE version = $1 AND ordinal = $2 AND exact_sha256 = $3 AND state = 'started'",
      version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256));
}

void complete_started_unit(pqxx::transaction_base& transaction, const std::string_view version,
                           const ExecutionUnit& unit, const std::string_view after_schema_sha256,
                           const bool final_unit, const std::string_view exact_file_sha256) {
  if (final_unit) {
    const auto rows = execute_parameters(
        transaction,
        "WITH completed AS ("
        "  UPDATE \"_dbdiff\".\"units\" "
        "  SET state = 'completed', after_schema_sha256 = $4, "
        "      updated_at = pg_catalog.clock_timestamp() "
        "  WHERE version = $1 AND ordinal = $2 AND exact_sha256 = $3 "
        "    AND state = 'started' RETURNING 1"
        ") "
        "UPDATE \"_dbdiff\".\"migrations\" "
        "SET completed_file_sha256 = $5, attempted_file_sha256 = $5, "
        "    updated_at = pg_catalog.clock_timestamp() "
        "WHERE version = $1 AND EXISTS (SELECT 1 FROM completed) RETURNING version",
        version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256, after_schema_sha256,
        exact_file_sha256);
    if (rows.size() != 1U) {
      throw Error{ErrorCode::drift,
                  "PostgreSQL could not atomically checkpoint the final standalone unit"};
    }
    return;
  }
  const auto rows = execute_parameters(
      transaction,
      "UPDATE \"_dbdiff\".\"units\" "
      "SET state = 'completed', after_schema_sha256 = $4, "
      "    updated_at = pg_catalog.clock_timestamp() "
      "WHERE version = $1 AND ordinal = $2 AND exact_sha256 = $3 AND state = 'started' "
      "RETURNING ordinal",
      version, static_cast<std::uint64_t>(unit.ordinal), unit.exact_sha256, after_schema_sha256);
  if (rows.size() != 1U) {
    throw Error{ErrorCode::drift,
                "PostgreSQL could not checkpoint a completed standalone migration unit"};
  }
}

void complete_empty_migration(pqxx::transaction_base& transaction, const std::string_view version,
                              const std::string_view exact_file_sha256) {
  const auto rows =
      execute_parameters(transaction,
                         "UPDATE \"_dbdiff\".\"migrations\" "
                         "SET completed_file_sha256 = $2, attempted_file_sha256 = $2, "
                         "    updated_at = pg_catalog.clock_timestamp() "
                         "WHERE version = $1 AND completed_file_sha256 IS NULL RETURNING version",
                         version, exact_file_sha256);
  if (rows.size() != 1U) {
    throw Error{ErrorCode::drift, "PostgreSQL could not complete an empty migration"};
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

} // namespace

struct Database::Impl {
  std::unique_ptr<pqxx::connection> connection;
  ServerVersion version;

  Impl(std::unique_ptr<pqxx::connection> database_connection, const ServerVersion server_version)
      : connection{std::move(database_connection)}, version{server_version} {}
};

Database::Database(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

Database::~Database() = default;

Database::Database(Database&&) noexcept = default;

Database& Database::operator=(Database&&) noexcept = default;

Database Database::open(const ConnectionLocator& locator) {
  try {
    const auto connection_string = locator.connection_string();
    auto connection = std::make_unique<pqxx::connection>(connection_string.c_str());
    pqxx::nontransaction transaction{*connection};
    const auto version = connection_server_version(transaction);
    execute_no_rows(transaction, "SET standard_conforming_strings TO on");
    return Database{std::make_unique<Impl>(std::move(connection), version)};
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw_connection_error("connection");
  }
}

Database Database::open(const std::string_view locator) {
  return open(ConnectionLocator::parse(locator));
}

bool Database::is_open() const noexcept {
  return implementation_ && implementation_->connection && implementation_->connection->is_open();
}

const ServerVersion& Database::server_version() const {
  if (!is_open()) {
    throw std::logic_error{"PostgreSQL database session is not open"};
  }
  return implementation_->version;
}

SchemaSnapshot Database::introspect(const std::vector<std::string>& managed_schemas) const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  try {
    return introspect_connection(*implementation_->connection, managed_schemas);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw_connection_error("schema introspection");
  }
}

void Database::execute_source(const std::string_view sql) {
  execute_sources(std::vector<std::string_view>{sql});
}

void Database::execute_sources(const std::vector<std::string_view>& sources) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }

  std::vector<std::vector<StatementSpan>> source_statements;
  source_statements.reserve(sources.size());
  for (const auto source : sources) {
    auto statements = scan_statements(source);
    validate_source_statements(source, statements);
    source_statements.push_back(std::move(statements));
  }

  try {
    pqxx::work transaction{*implementation_->connection};
    for (std::size_t source_index = 0U; source_index < sources.size(); ++source_index) {
      for (const auto& statement : source_statements[source_index]) {
        execute_no_rows(transaction, sources[source_index].substr(statement.begin,
                                                                  statement.end - statement.begin));
      }
    }
    transaction.commit();
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::execution, "PostgreSQL declarative source execution failed"};
  }
}

void Database::execute_sources(const SourceSet& sources) {
  std::vector<std::string_view> sql;
  sql.reserve(sources.files.size());
  for (const auto& source : sources.files) {
    sql.emplace_back(source.sql);
  }
  execute_sources(sql);
}

void Database::execute_migration(const std::string_view sql) {
  const auto parsed = parse_migration(std::string{sql});
  execute_prefix(sql, parsed.units.size());
}

void Database::execute_prefix(const std::string_view sql, const std::size_t completed_unit_count) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  const auto parsed = parse_migration(std::string{sql});
  if (completed_unit_count > parsed.units.size()) {
    throw Error{ErrorCode::migration,
                "PostgreSQL execution prefix exceeds the migration unit count"};
  }

  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    execute_unit_prefix(transaction, parsed, completed_unit_count);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::execution, "PostgreSQL migration execution failed"};
  }
}

MigrationHistory Database::read_history() const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    return read_history_unlocked(transaction);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::database, "PostgreSQL migration history read failed"};
  }
}

MigrationApplyResult Database::apply_version(const std::string_view version,
                                             const std::string_view exact_file_sha256,
                                             const std::string_view sql, const bool resume) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  require_version(version);
  require_file_sha256(exact_file_sha256);
  if (!is_valid_utf8(sql)) {
    throw Error{ErrorCode::migration, "PostgreSQL migration SQL must be valid UTF-8"};
  }
  const auto parsed = parse_migration(std::string{sql});
  validate_resumable_session_units(parsed);

  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    ensure_history_tables(transaction);
    const auto history = read_history_unlocked(transaction);
    const auto existing = history_entry(history, version);
    validate_history_for_apply(history, version, existing.has_value());
    auto result = validate_migration_resume(existing, parsed, exact_file_sha256, resume);
    if (result.already_completed) {
      return result;
    }
    if (existing.has_value() &&
        parse_server_version(existing->engine_version).major != implementation_->version.major) {
      throw Error{ErrorCode::unsupported,
                  "cannot resume a PostgreSQL migration attempt from another server major"};
    }

    const auto revision_ordinal =
        record_revision(transaction, version, std::to_string(implementation_->version.number),
                        exact_file_sha256, sql);
    if (parsed.units.empty()) {
      complete_empty_migration(transaction, version, exact_file_sha256);
      return MigrationApplyResult{
          .already_completed = false,
          .completed_unit_count = 0U,
          .completed_file_sha256 = std::string{exact_file_sha256},
      };
    }

    for (std::size_t index = 0U; index < result.completed_unit_count; ++index) {
      const auto& unit = parsed.units[index];
      if (!replayable_session_unit(parsed, unit)) {
        continue;
      }
      const auto before_schema_sha256 = catalog_fingerprint(transaction);
      execute_plain_unit(transaction, parsed, unit);
      if (catalog_fingerprint(transaction) != before_schema_sha256) {
        throw Error{ErrorCode::drift,
                    "replayed PostgreSQL session state unexpectedly changed the schema"};
      }
    }

    for (std::size_t index = result.completed_unit_count; index < parsed.units.size(); ++index) {
      const auto& unit = parsed.units[index];
      const auto final_unit = index + 1U == parsed.units.size();
      const auto before_schema_sha256 = catalog_fingerprint(transaction);
      if (replayable_session_unit(parsed, unit)) {
        execute_plain_unit(transaction, parsed, unit);
        const auto after_schema_sha256 = catalog_fingerprint(transaction);
        if (after_schema_sha256 != before_schema_sha256) {
          throw Error{ErrorCode::drift,
                      "PostgreSQL session state unexpectedly changed the persistent schema"};
        }
        checkpoint_completed_unit(transaction, version, unit, before_schema_sha256,
                                  after_schema_sha256, revision_ordinal, final_unit,
                                  exact_file_sha256);
      } else if (unit.explicit_transaction) {
        execute_transaction_unit(transaction, parsed, unit, [&] {
          const auto after_schema_sha256 = catalog_fingerprint(transaction);
          checkpoint_completed_unit(transaction, version, unit, before_schema_sha256,
                                    after_schema_sha256, revision_ordinal, final_unit,
                                    exact_file_sha256);
        });
      } else {
        checkpoint_started_unit(transaction, version, unit, before_schema_sha256, revision_ordinal);
        try {
          execute_plain_unit(transaction, parsed, unit);
        } catch (const pqxx::sql_error&) {
          try {
            remove_failed_started_unit(transaction, version, unit);
          } catch (const std::exception& cleanup_error) {
            static_cast<void>(cleanup_error);
          }
          throw Error{ErrorCode::execution,
                      "PostgreSQL standalone migration unit failed before completion"};
        }
        const auto after_schema_sha256 = catalog_fingerprint(transaction);
        complete_started_unit(transaction, version, unit, after_schema_sha256, final_unit,
                              exact_file_sha256);
      }
      result.completed_unit_count = index + 1U;
    }
    result.completed_file_sha256 = exact_file_sha256;
    return result;
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::execution,
                "PostgreSQL migration apply failed; inspect history before retrying"};
  }
}

std::vector<MigrationRevision> Database::recover_revisions(const std::string_view version) const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  require_version(version);
  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    SessionAdvisoryLock lock{transaction};
    if (!history_tables_initialized(transaction)) {
      return {};
    }
    const auto rows =
        execute_parameters(transaction,
                           "SELECT ordinal, exact_file_sha256, sql "
                           "FROM \"_dbdiff\".\"revisions\" WHERE version = $1 ORDER BY ordinal",
                           version);
    std::vector<MigrationRevision> revisions;
    revisions.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
      auto exact_hash = row[1].as<std::string>();
      validate_stored_hash(exact_hash, "revision file hash");
      const auto ordinal = history_ordinal(row[0], "migration revision ordinal");
      auto exact_sql = row[2].as<std::string>();
      if (ordinal != revisions.size() || sha256_hex(exact_sql) != exact_hash) {
        throw Error{ErrorCode::drift,
                    "PostgreSQL migration revisions are not exact and contiguous"};
      }
      revisions.push_back(MigrationRevision{
          .ordinal = ordinal,
          .exact_file_sha256 = std::move(exact_hash),
          .sql = std::move(exact_sql),
      });
    }
    return revisions;
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::database, "PostgreSQL migration revision recovery failed"};
  }
}

struct ScratchDatabase::Impl {
  ConnectionLocator provisioning_locator;
  ScratchIdentity identity;
  ServerVersion version;
  std::uint64_t owner_oid{};
  std::unique_ptr<Database> database;
  bool closed{false};

  Impl(ConnectionLocator provisioning_locator_value, ScratchIdentity identity_value,
       const ServerVersion server_version, const std::uint64_t provisioning_owner_oid,
       Database database_session)
      : provisioning_locator{std::move(provisioning_locator_value)},
        identity{std::move(identity_value)}, version{server_version},
        owner_oid{provisioning_owner_oid},
        database{std::make_unique<Database>(std::move(database_session))} {}

  ~Impl() {
    if (!closed) {
      try {
        close();
      } catch (const std::exception&) {
        closed = true;
      }
    }
  }

  void close() {
    if (closed) {
      return;
    }
    database.reset();
    drop_scratch_database(provisioning_locator, identity, owner_oid);
    closed = true;
  }
};

ScratchDatabase::ScratchDatabase(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

ScratchDatabase ScratchDatabase::create(const ConnectionLocator& provisioning_locator) {
  const auto identity = ScratchIdentity::generate();
  std::uint64_t owner_oid{};
  ServerVersion version{};
  bool database_created = false;

  try {
    const auto connection_string = provisioning_locator.connection_string();
    pqxx::connection provisioning_connection{connection_string.c_str()};
    {
      pqxx::nontransaction transaction{provisioning_connection};
      version = connection_server_version(transaction);
      owner_oid = provisioning_owner(transaction);
      if (execute_one_row(transaction, "SELECT pg_catalog.current_database()")[0]
              .as<std::string>() == identity.name.value()) {
        throw Error{ErrorCode::configuration,
                    "the PostgreSQL provisioning database cannot be the scratch database"};
      }

      execute_no_rows(transaction,
                      "CREATE DATABASE " + identity.name.quoted() + " TEMPLATE template0");
      database_created = true;
      execute_no_rows(transaction, "COMMENT ON DATABASE " + identity.name.quoted() + " IS " +
                                       identity.marker.quoted());
    }

    const auto database_locator = provisioning_locator.with_database(identity.name.value());
    auto database = Database::open(database_locator);
    return ScratchDatabase{std::make_unique<Impl>(provisioning_locator, identity, version,
                                                  owner_oid, std::move(database))};
  } catch (const Error& error) {
    if (database_created && !try_creation_cleanup(provisioning_locator, identity, owner_oid)) {
      throw Error{error.code(),
                  std::string{error.what()} + "; automatic scratch cleanup also failed"};
    }
    throw;
  } catch (const std::exception&) {
    if (database_created && !try_creation_cleanup(provisioning_locator, identity, owner_oid)) {
      throw Error{ErrorCode::database,
                  "PostgreSQL scratch database creation and automatic cleanup failed"};
    }
    throw_connection_error("scratch database creation");
  }
}

ScratchDatabase ScratchDatabase::create(const std::string_view provisioning_locator) {
  return create(ConnectionLocator::parse(provisioning_locator));
}

ScratchDatabase::ScratchDatabase(ScratchDatabase&&) noexcept = default;

ScratchDatabase& ScratchDatabase::operator=(ScratchDatabase&&) noexcept = default;

ScratchDatabase::~ScratchDatabase() = default;

const ScratchIdentity& ScratchDatabase::identity() const {
  if (!implementation_) {
    throw std::logic_error{"PostgreSQL scratch database has been moved"};
  }
  return implementation_->identity;
}

const ServerVersion& ScratchDatabase::server_version() const {
  if (!implementation_) {
    throw std::logic_error{"PostgreSQL scratch database has been moved"};
  }
  return implementation_->version;
}

bool ScratchDatabase::is_open() const noexcept {
  return implementation_ && !implementation_->closed && implementation_->database &&
         implementation_->database->is_open();
}

SchemaSnapshot ScratchDatabase::introspect(const std::vector<std::string>& managed_schemas) const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  return implementation_->database->introspect(managed_schemas);
}

void ScratchDatabase::execute_source(const std::string_view sql) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  implementation_->database->execute_source(sql);
}

void ScratchDatabase::execute_sources(const std::vector<std::string_view>& sources) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  implementation_->database->execute_sources(sources);
}

void ScratchDatabase::execute_sources(const SourceSet& sources) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  implementation_->database->execute_sources(sources);
}

void ScratchDatabase::execute_migration(const std::string_view sql) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  implementation_->database->execute_migration(sql);
}

void ScratchDatabase::execute_prefix(const std::string_view sql,
                                     const std::size_t completed_unit_count) {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  implementation_->database->execute_prefix(sql, completed_unit_count);
}

void ScratchDatabase::close() {
  if (implementation_) {
    try {
      implementation_->close();
    } catch (const Error&) {
      throw;
    } catch (const std::exception&) {
      throw_connection_error("scratch database cleanup");
    }
  }
}

} // namespace dbdiff::postgresql
