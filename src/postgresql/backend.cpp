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

[[nodiscard]] ConstraintKind parse_constraint_kind(const std::string_view value,
                                                   const int server_major) {
  if (value == "p") {
    return ConstraintKind::primary_key;
  }
  if (value == "u") {
    return ConstraintKind::unique;
  }
  if (value == "c") {
    return ConstraintKind::check;
  }
  if (value == "f") {
    return ConstraintKind::foreign_key;
  }
  if (value == "n" && server_major >= 18) {
    return ConstraintKind::not_null;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL table constraint kind"};
}

[[nodiscard]] PolicyCommand parse_policy_command(const std::string_view value) {
  if (value == "*") {
    return PolicyCommand::all;
  }
  if (value == "r") {
    return PolicyCommand::select;
  }
  if (value == "a") {
    return PolicyCommand::insert;
  }
  if (value == "w") {
    return PolicyCommand::update;
  }
  if (value == "d") {
    return PolicyCommand::delete_rows;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL row-security policy command"};
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

void append_unsupported_catalog_objects(std::vector<UnsupportedCatalogObject>& objects,
                                        const pqxx::result& rows) {
  for (const auto& row : rows) {
    if (row.size() != 2U || row[0].is_null() || row[1].is_null()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL catalog returned an invalid unsupported-object record"};
    }
    objects.push_back(UnsupportedCatalogObject{
        .kind = row[0].as<std::string>(),
        .identity = row[1].as<std::string>(),
    });
  }
}

[[nodiscard]] std::vector<UnsupportedCatalogObject>
unsupported_catalog_objects(pqxx::transaction_base& transaction, const std::string& filter,
                            const int server_major) {
  std::vector<UnsupportedCatalogObject> objects;
  const std::string qualified =
      "pg_catalog.quote_ident(n.nspname) || '.' || pg_catalog.quote_ident(c.relname)";

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT CASE c.relkind "
          "         WHEN 'p' THEN 'partitioned table' WHEN 'v' THEN 'view' "
          "         WHEN 'm' THEN 'materialized view' WHEN 'S' THEN 'sequence' "
          "         WHEN 'f' THEN 'foreign table' WHEN 'I' THEN 'partitioned index' "
          "         WHEN 'c' THEN 'composite relation' ELSE 'relation kind ' || c.relkind::text "
          "       END, " +
          qualified +
          " FROM pg_catalog.pg_class AS c "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND ("
          "  c.relkind NOT IN ('r', 'i', 'S') OR c.relispartition "
          "  OR (c.relkind = 'r' AND c.relpersistence NOT IN ('p', 'u')) "
          "  OR (c.relkind = 'S' AND NOT EXISTS ("
          "    SELECT 1 FROM pg_catalog.pg_depend AS d "
          "    WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
          "      AND d.objid = c.oid "
          "      AND d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
          "      AND d.deptype = 'i'))"
          ")"));

  append_unsupported_catalog_objects(
      objects, transaction.exec("SELECT 'table inheritance', " + qualified +
                                " FROM pg_catalog.pg_class AS c "
                                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                                "WHERE n.nspname IN (" +
                                filter +
                                ") AND c.relkind = 'r' AND EXISTS ("
                                "  SELECT 1 FROM pg_catalog.pg_inherits AS i "
                                "  WHERE i.inhrelid = c.oid OR i.inhparent = c.oid)"));

  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   "SELECT 'table storage property', " + qualified +
                   " FROM pg_catalog.pg_class AS c "
                   "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                   "LEFT JOIN pg_catalog.pg_am AS am ON am.oid = c.relam "
                   "WHERE n.nspname IN (" +
                   filter +
                   ") AND c.relkind = 'r' AND ("
                   "  c.reloptions IS NOT NULL OR c.reltablespace <> 0 OR c.relreplident <> 'd' "
                   "  OR c.reloftype <> 0 OR am.amname IS DISTINCT FROM 'heap')"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'ownership or privileges', " + qualified +
                       " FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND (c.relacl IS NOT NULL OR c.relowner <> "
                       "  (SELECT r.oid FROM pg_catalog.pg_roles AS r "
                       "   WHERE r.rolname = CURRENT_USER)) "
                       "UNION ALL "
                       "SELECT 'schema ownership or privileges', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_namespace AS n "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND ((n.nspname <> 'public' AND (n.nspacl IS NOT NULL OR n.nspowner <> "
                       "    (SELECT r.oid FROM pg_catalog.pg_roles AS r "
                       "     WHERE r.rolname = CURRENT_USER))) "
                       " OR (n.nspname = 'public' AND NOT ("
                       "    n.nspowner = (SELECT r.oid FROM pg_catalog.pg_roles AS r "
                       "                    WHERE r.rolname = 'pg_database_owner') "
                       "    AND (SELECT pg_catalog.count(*) FROM "
                       "           pg_catalog.aclexplode(n.nspacl)) = 3 "
                       "    AND NOT EXISTS (SELECT 1 FROM pg_catalog.aclexplode(n.nspacl) AS acl "
                       "      WHERE acl.grantor <> n.nspowner OR acl.is_grantable "
                       "         OR NOT ((acl.grantee = n.nspowner "
                       "                  AND acl.privilege_type IN ('CREATE', 'USAGE')) "
                       "             OR (acl.grantee = 0 AND acl.privilege_type = 'USAGE')))))) "
                       "UNION ALL "
                       "SELECT 'default privileges', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_default_acl AS a "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = a.defaclnamespace "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'column storage or privileges', " + qualified +
                       " || '.' || pg_catalog.quote_ident(a.attname) "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped AND ("
                       "  a.attstattarget <> -1 OR a.attstorage <> t.typstorage "
                       "  OR a.attcompression <> ''::pg_catalog.\"char\" OR a.attacl IS NOT NULL "
                       "  OR a.attoptions IS NOT NULL OR a.attfdwoptions IS NOT NULL)"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'external column type', " + qualified +
          " || '.' || pg_catalog.quote_ident(a.attname) || ' -> ' || "
          "       pg_catalog.quote_ident(tn.nspname) || '.' || pg_catalog.quote_ident(t.typname) "
          "FROM pg_catalog.pg_attribute AS a "
          "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid "
          "JOIN pg_catalog.pg_namespace AS tn ON tn.oid = t.typnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
          "  AND tn.nspname <> 'pg_catalog' "
          "UNION ALL "
          "SELECT 'external column collation', " +
          qualified +
          " || '.' || pg_catalog.quote_ident(a.attname) || ' -> ' || "
          "       pg_catalog.quote_ident(cn.nspname) || '.' || "
          "pg_catalog.quote_ident(coll.collname) "
          "FROM pg_catalog.pg_attribute AS a "
          "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "JOIN pg_catalog.pg_collation AS coll ON coll.oid = a.attcollation "
          "JOIN pg_catalog.pg_namespace AS cn ON cn.oid = coll.collnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
          "  AND cn.nspname <> 'pg_catalog'"));

  const std::string expression_sources =
      "WITH sources(kind, identity, classid, objid) AS ("
      " SELECT 'column expression', " +
      qualified +
      " || '.' || pg_catalog.quote_ident(a.attname), "
      "        'pg_catalog.pg_attrdef'::pg_catalog.regclass, ad.oid "
      " FROM pg_catalog.pg_attrdef AS ad "
      " JOIN pg_catalog.pg_attribute AS a "
      "   ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
      " JOIN pg_catalog.pg_class AS c ON c.oid = ad.adrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter +
      ") AND c.relkind = 'r' "
      " UNION ALL "
      " SELECT 'constraint expression', " +
      qualified +
      " || '.' || pg_catalog.quote_ident(con.conname), "
      "        'pg_catalog.pg_constraint'::pg_catalog.regclass, con.oid "
      " FROM pg_catalog.pg_constraint AS con "
      " JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter +
      ") "
      " UNION ALL "
      " SELECT 'index expression', " +
      qualified +
      ", 'pg_catalog.pg_class'::pg_catalog.regclass, c.oid "
      " FROM pg_catalog.pg_index AS i "
      " JOIN pg_catalog.pg_class AS c ON c.oid = i.indexrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter +
      ") "
      " UNION ALL "
      " SELECT 'policy expression', " +
      qualified +
      " || '.' || pg_catalog.quote_ident(p.polname), "
      "        'pg_catalog.pg_policy'::pg_catalog.regclass, p.oid "
      " FROM pg_catalog.pg_policy AS p "
      " JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter + ") ) ";
  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   expression_sources +
                   "SELECT s.kind || ' external dependency', s.identity || ' -> ' || "
                   "       pg_catalog.pg_describe_object(d.refclassid, d.refobjid, d.refobjsubid) "
                   "FROM sources AS s "
                   "JOIN pg_catalog.pg_depend AS d ON d.classid = s.classid AND d.objid = s.objid "
                   "LEFT JOIN pg_catalog.pg_proc AS p ON d.refclassid = "
                   "  'pg_catalog.pg_proc'::pg_catalog.regclass AND p.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS pn ON pn.oid = p.pronamespace "
                   "LEFT JOIN pg_catalog.pg_type AS t ON d.refclassid = "
                   "  'pg_catalog.pg_type'::pg_catalog.regclass AND t.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS tn ON tn.oid = t.typnamespace "
                   "LEFT JOIN pg_catalog.pg_operator AS o ON d.refclassid = "
                   "  'pg_catalog.pg_operator'::pg_catalog.regclass AND o.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS onsp ON onsp.oid = o.oprnamespace "
                   "LEFT JOIN pg_catalog.pg_collation AS coll ON d.refclassid = "
                   "  'pg_catalog.pg_collation'::pg_catalog.regclass AND coll.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS cn ON cn.oid = coll.collnamespace "
                   "LEFT JOIN pg_catalog.pg_opclass AS opc ON d.refclassid = "
                   "  'pg_catalog.pg_opclass'::pg_catalog.regclass AND opc.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS opcn ON opcn.oid = opc.opcnamespace "
                   "LEFT JOIN pg_catalog.pg_opfamily AS opf ON d.refclassid = "
                   "  'pg_catalog.pg_opfamily'::pg_catalog.regclass AND opf.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS opfn ON opfn.oid = opf.opfnamespace "
                   "WHERE (pn.oid IS NOT NULL AND pn.nspname <> 'pg_catalog') "
                   "   OR (tn.oid IS NOT NULL AND tn.nspname <> 'pg_catalog') "
                   "   OR (onsp.oid IS NOT NULL AND onsp.nspname <> 'pg_catalog') "
                   "   OR (cn.oid IS NOT NULL AND cn.nspname <> 'pg_catalog') "
                   "   OR (opcn.oid IS NOT NULL AND opcn.nspname <> 'pg_catalog') "
                   "   OR (opfn.oid IS NOT NULL AND opfn.nspname <> 'pg_catalog')"));

  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   "SELECT 'column expression relation dependency', " + qualified +
                   " || '.' || pg_catalog.quote_ident(a.attname) || ' -> ' || "
                   "       pg_catalog.pg_describe_object(d.refclassid, d.refobjid, d.refobjsubid) "
                   "FROM pg_catalog.pg_attrdef AS ad "
                   "JOIN pg_catalog.pg_attribute AS a "
                   "  ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
                   "JOIN pg_catalog.pg_class AS c ON c.oid = ad.adrelid "
                   "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                   "JOIN pg_catalog.pg_depend AS d ON d.classid = "
                   "  'pg_catalog.pg_attrdef'::pg_catalog.regclass AND d.objid = ad.oid "
                   "WHERE n.nspname IN (" +
                   filter +
                   ") AND c.relkind = 'r' "
                   "  AND d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                   "  AND d.refobjid <> ad.adrelid"));

  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   "SELECT 'index state or storage property', " + qualified +
                   " FROM pg_catalog.pg_index AS i "
                   "JOIN pg_catalog.pg_class AS c ON c.oid = i.indexrelid "
                   "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                   "JOIN pg_catalog.pg_am AS am ON am.oid = c.relam "
                   "WHERE n.nspname IN (" +
                   filter +
                   ") AND (NOT i.indisvalid OR NOT i.indisready OR NOT i.indislive "
                   "  OR i.indcheckxmin OR i.indisclustered OR i.indisreplident "
                   "  OR i.indisexclusion OR c.reloptions IS NOT NULL OR c.reltablespace <> 0 "
                   "  OR am.amname NOT IN ('btree', 'hash', 'gist', 'spgist', 'gin', 'brin'))"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'identity sequence dependency', " + qualified +
                       " || '.' || pg_catalog.quote_ident(a.attname) "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "LEFT JOIN pg_catalog.pg_depend AS d "
                       "  ON d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND d.refobjid = c.oid AND d.refobjsubid = a.attnum AND d.deptype = 'i' "
                       "LEFT JOIN pg_catalog.pg_class AS seq "
                       "  ON d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND seq.oid = d.objid AND seq.relkind = 'S' "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
                       "  AND a.attidentity <> '' "
                       "GROUP BY n.nspname, c.relname, a.attname "
                       "HAVING pg_catalog.count(seq.oid) <> 1"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'identity sequence properties', " + qualified +
                       " || '.' || pg_catalog.quote_ident(a.attname) "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_depend AS d "
                       "  ON d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND d.refobjid = c.oid AND d.refobjsubid = a.attnum AND d.deptype = 'i' "
                       "JOIN pg_catalog.pg_class AS seq "
                       "  ON d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND seq.oid = d.objid AND seq.relkind = 'S' "
                       "JOIN pg_catalog.pg_namespace AS sn ON sn.oid = seq.relnamespace "
                       "JOIN pg_catalog.pg_sequence AS s ON s.seqrelid = seq.oid "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
                       "  AND a.attidentity <> '' AND ("
                       "    sn.oid <> n.oid OR seq.relpersistence <> c.relpersistence "
                       "    OR pg_catalog.octet_length(c.relname || '_' || a.attname || '_seq') > "
                       "       pg_catalog.current_setting('max_identifier_length')::integer "
                       "    OR seq.relname <> c.relname || '_' || a.attname || '_seq' "
                       "    OR a.atttypid NOT IN ('pg_catalog.int2'::pg_catalog.regtype, "
                       "                            'pg_catalog.int4'::pg_catalog.regtype, "
                       "                            'pg_catalog.int8'::pg_catalog.regtype) "
                       "    OR s.seqtypid <> a.atttypid OR s.seqstart <> 1 OR s.seqincrement <> 1 "
                       "    OR s.seqmin <> 1 "
                       "    OR s.seqmax <> CASE a.atttypid "
                       "         WHEN 'pg_catalog.int2'::pg_catalog.regtype THEN 32767 "
                       "         WHEN 'pg_catalog.int4'::pg_catalog.regtype THEN 2147483647 "
                       "         ELSE 9223372036854775807 END "
                       "    OR s.seqcache <> 1 OR s.seqcycle OR seq.reloptions IS NOT NULL "
                       "    OR seq.reltablespace <> 0)"));

  append_unsupported_catalog_objects(
      objects, transaction.exec("SELECT 'internal trigger state', " + qualified +
                                " || '.' || pg_catalog.quote_ident(t.tgname) "
                                "FROM pg_catalog.pg_trigger AS t "
                                "JOIN pg_catalog.pg_class AS c ON c.oid = t.tgrelid "
                                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                                "WHERE n.nspname IN (" +
                                filter + ") AND t.tgisinternal AND t.tgenabled <> 'O'"));

  const auto supported_constraints = server_major >= 18 ? std::string{"('p', 'u', 'c', 'f', 'n')"}
                                                        : std::string{"('p', 'u', 'c', 'f')"};
  std::string unsupported_constraint_properties =
      "con.contype NOT IN " + supported_constraints +
      " OR con.contypid <> 0 OR con.conparentid <> 0 OR con.coninhcount <> 0 "
      "OR (con.contype = 'f' AND NOT EXISTS ("
      "  SELECT 1 FROM pg_catalog.pg_class AS rc "
      "  JOIN pg_catalog.pg_namespace AS rn ON rn.oid = rc.relnamespace "
      "  WHERE rc.oid = con.confrelid AND rn.nspname IN (" +
      filter + ") AND rc.relkind = 'r' AND NOT rc.relispartition))";
  if (server_major >= 18) {
    unsupported_constraint_properties += " OR con.conperiod";
  }
  append_unsupported_catalog_objects(
      objects, transaction.exec("SELECT 'constraint kind or dependency', "
                                "       pg_catalog.quote_ident(n.nspname) || '.' || "
                                "       pg_catalog.quote_ident(c.relname) || '.' || "
                                "       pg_catalog.quote_ident(con.conname) "
                                "FROM pg_catalog.pg_constraint AS con "
                                "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                                "WHERE n.nspname IN (" +
                                filter + ") AND (" + unsupported_constraint_properties + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'constraint backing index identity', "
                       "       pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) || '.' || "
                       "       pg_catalog.quote_ident(con.conname) || ' -> ' || "
                       "       pg_catalog.quote_ident(idx.relname) "
                       "FROM pg_catalog.pg_constraint AS con "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_class AS idx ON idx.oid = con.conindid "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND con.contype IN ('p', 'u') "
                       "  AND (idx.relnamespace <> c.relnamespace OR idx.relname <> con.conname)"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'extension-owned relation', " + qualified +
                       " FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_class AS c ON d.classid = "
                       "  'pg_catalog.pg_class'::pg_catalog.regclass AND c.oid = d.objid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extension-owned schema', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_namespace AS n ON d.classid = "
                       "  'pg_catalog.pg_namespace'::pg_catalog.regclass AND n.oid = d.objid "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extension-owned constraint', "
                       "       pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) || '.' || "
                       "       pg_catalog.quote_ident(con.conname) "
                       "FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_constraint AS con ON d.classid = "
                       "  'pg_catalog.pg_constraint'::pg_catalog.regclass AND con.oid = d.objid "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extension-owned policy', "
                       "       pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) || '.' || "
                       "       pg_catalog.quote_ident(p.polname) "
                       "FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_policy AS p ON d.classid = "
                       "  'pg_catalog.pg_policy'::pg_catalog.regclass AND p.oid = d.objid "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'type', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(t.typname) "
          "FROM pg_catalog.pg_type AS t "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = t.typnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND NOT EXISTS ("
          "  SELECT 1 FROM pg_catalog.pg_class AS c "
          "  WHERE c.reltype = t.oid AND c.relkind = 'r' AND NOT c.relispartition) "
          "AND NOT EXISTS ("
          "  SELECT 1 FROM pg_catalog.pg_type AS row_type "
          "  JOIN pg_catalog.pg_class AS c ON c.reltype = row_type.oid "
          "  WHERE row_type.typarray = t.oid AND c.relkind = 'r' AND NOT c.relispartition)"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'routine', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(p.proname) || '(' || "
                       "       pg_catalog.pg_get_function_identity_arguments(p.oid) || ')' "
                       "FROM pg_catalog.pg_proc AS p "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'trigger', " +
                       qualified +
                       " || '.' || pg_catalog.quote_ident(t.tgname) "
                       "FROM pg_catalog.pg_trigger AS t "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = t.tgrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE NOT t.tgisinternal AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'rule', " +
                       qualified +
                       " || '.' || pg_catalog.quote_ident(r.rulename) "
                       "FROM pg_catalog.pg_rewrite AS r "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = r.ev_class "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE r.rulename <> '_RETURN' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extended statistics', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(s.stxname) "
                       "FROM pg_catalog.pg_statistic_ext AS s "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = s.stxnamespace "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'text search configuration', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.cfgname) "
          "FROM pg_catalog.pg_ts_config AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.cfgnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'text search dictionary', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.dictname) "
          "FROM pg_catalog.pg_ts_dict AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.dictnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'text search parser', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.prsname) "
          "FROM pg_catalog.pg_ts_parser AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.prsnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'text search template', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.tmplname) "
          "FROM pg_catalog.pg_ts_template AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.tmplnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'publication membership', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) || ' in ' || "
          "       pg_catalog.quote_ident(p.pubname) "
          "FROM pg_catalog.pg_publication_rel AS pr "
          "JOIN pg_catalog.pg_publication AS p ON p.oid = pr.prpubid "
          "JOIN pg_catalog.pg_class AS c ON c.oid = pr.prrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'security label', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) "
                       "FROM pg_catalog.pg_seclabel AS s "
                       "JOIN pg_catalog.pg_class AS c ON s.classoid = "
                       "  'pg_catalog.pg_class'::pg_catalog.regclass AND c.oid = s.objoid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'security label', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_seclabel AS s "
                       "JOIN pg_catalog.pg_namespace AS n ON s.classoid = "
                       "  'pg_catalog.pg_namespace'::pg_catalog.regclass AND n.oid = s.objoid "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'collation', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.collname) "
                       "FROM pg_catalog.pg_collation AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.collnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'conversion', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.conname) "
                       "FROM pg_catalog.pg_conversion AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.connamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'operator', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.oprname) "
                       "FROM pg_catalog.pg_operator AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.oprnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'operator class', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.opcname) "
                       "FROM pg_catalog.pg_opclass AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.opcnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'operator family', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.opfname) "
                       "FROM pg_catalog.pg_opfamily AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.opfnamespace "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) "
          "       || CASE WHEN d.objsubid = 0 THEN '' ELSE '.column#' || d.objsubid::text END "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_class AS c ON d.classoid = "
          "  'pg_catalog.pg_class'::pg_catalog.regclass AND c.oid = d.objoid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_namespace AS n ON d.classoid = "
          "  'pg_catalog.pg_namespace'::pg_catalog.regclass AND n.oid = d.objoid "
          "WHERE n.nspname IN (" +
          filter +
          ") AND NOT (n.nspname = 'public' AND "
          "               d.description = 'standard public schema') "
          "UNION ALL "
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) || '.' || "
          "       pg_catalog.quote_ident(con.conname) "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_constraint AS con ON d.classoid = "
          "  'pg_catalog.pg_constraint'::pg_catalog.regclass AND con.oid = d.objoid "
          "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) || '.' || "
          "       pg_catalog.quote_ident(p.polname) "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_policy AS p ON d.classoid = "
          "  'pg_catalog.pg_policy'::pg_catalog.regclass AND p.oid = d.objoid "
          "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter + ")"));

  return objects;
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
  hash.add_length_prefixed("dbdiff.postgresql.catalog-checkpoint.v2");
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
  hash_result(
      hash, "constraints",
      transaction.exec("SELECT n.nspname, c.relname, con.conname, con.contype::text, "
                       "       pg_catalog.pg_get_constraintdef(con.oid, false), con.convalidated "
                       "FROM pg_catalog.pg_constraint AS con "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "          con.conname COLLATE \"C\""));
  hash_result(
      hash, "indexes",
      transaction.exec("SELECT n.nspname, c.relname, pg_catalog.pg_get_indexdef(c.oid, 0, false) "
                       "FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " AND c.relkind IN ('i', 'I') "
                       "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\""));
  hash_result(
      hash, "policies",
      transaction.exec("SELECT n.nspname, c.relname, p.polname, p.polcmd::text, p.polpermissive, "
                       "       p.polroles::text, p.polqual::text, p.polwithcheck::text "
                       "FROM pg_catalog.pg_policy AS p "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE " +
                       std::string{schema_predicate} +
                       " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "          p.polname COLLATE \"C\""));
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
  snapshot.unsupported_objects =
      unsupported_catalog_objects(transaction, filter, snapshot.server_version.major);
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

  const auto constraint_kinds = snapshot.server_version.major >= 18
                                    ? std::string{"('p', 'u', 'c', 'f', 'n')"}
                                    : std::string{"('p', 'u', 'c', 'f')"};
  const auto enforced_expression =
      snapshot.server_version.major >= 18 ? std::string{"con.conenforced"} : std::string{"TRUE"};
  const auto constraint_rows = transaction.exec(
      "SELECT n.nspname, c.relname, con.conname, con.contype::text, "
      "       pg_catalog.pg_get_constraintdef(con.oid, false), con.convalidated, " +
      enforced_expression +
      ", rn.nspname, rc.relname "
      "FROM pg_catalog.pg_constraint AS con "
      "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      "LEFT JOIN pg_catalog.pg_class AS rc ON rc.oid = con.confrelid "
      "LEFT JOIN pg_catalog.pg_namespace AS rn ON rn.oid = rc.relnamespace "
      "WHERE n.nspname IN (" +
      filter + ") AND c.relkind = 'r' AND NOT c.relispartition AND con.contype IN " +
      constraint_kinds +
      " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
      "          con.conname COLLATE \"C\"");

  using ConstraintIdentity = std::tuple<QualifiedName, std::string>;
  std::map<ConstraintIdentity, std::size_t> constraint_positions;
  snapshot.constraints.reserve(static_cast<std::size_t>(constraint_rows.size()));
  for (const auto& row : constraint_rows) {
    const QualifiedName table_name{row[0].as<std::string>(), row[1].as<std::string>()};
    std::optional<QualifiedName> referenced_table;
    if (!row[7].is_null() || !row[8].is_null()) {
      if (row[7].is_null() || row[8].is_null()) {
        throw Error{ErrorCode::database, "PostgreSQL returned an incomplete referenced-table name"};
      }
      referenced_table = QualifiedName{row[7].as<std::string>(), row[8].as<std::string>()};
    }
    const auto name = row[2].as<std::string>();
    const auto position = snapshot.constraints.size();
    if (!constraint_positions.emplace(ConstraintIdentity{table_name, name}, position).second) {
      throw Error{ErrorCode::database, "PostgreSQL returned a duplicate table constraint"};
    }
    snapshot.constraints.push_back(TableConstraint{
        .table = table_name,
        .name = name,
        .kind = parse_constraint_kind(row[3].as<std::string>(), snapshot.server_version.major),
        .columns = {},
        .definition = row[4].as<std::string>(),
        .validated = row[5].as<bool>(),
        .enforced = row[6].as<bool>(),
        .referenced_table = std::move(referenced_table),
        .referenced_columns = {},
    });
  }

  const auto constraint_column_rows = transaction.exec(
      "SELECT n.nspname, c.relname, con.conname, key.ordinality, a.attname "
      "FROM pg_catalog.pg_constraint AS con "
      "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      "CROSS JOIN LATERAL pg_catalog.unnest(con.conkey) WITH ORDINALITY "
      "  AS key(attnum, ordinality) "
      "JOIN pg_catalog.pg_attribute AS a "
      "  ON a.attrelid = con.conrelid AND a.attnum = key.attnum "
      "WHERE n.nspname IN (" +
      filter + ") AND c.relkind = 'r' AND NOT c.relispartition AND con.contype IN " +
      constraint_kinds +
      " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
      "          con.conname COLLATE \"C\", key.ordinality");
  for (const auto& row : constraint_column_rows) {
    const ConstraintIdentity identity{
        QualifiedName{row[0].as<std::string>(), row[1].as<std::string>()},
        row[2].as<std::string>()};
    const auto found = constraint_positions.find(identity);
    if (found == constraint_positions.end()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL returned a constraint column without its constraint"};
    }
    auto& columns = snapshot.constraints[found->second].columns;
    if (row[3].as<std::size_t>() != columns.size() + 1U) {
      throw Error{ErrorCode::database, "PostgreSQL constraint columns are not contiguous"};
    }
    columns.push_back(row[4].as<std::string>());
  }

  const auto referenced_column_rows =
      transaction.exec("SELECT n.nspname, c.relname, con.conname, key.ordinality, a.attname "
                       "FROM pg_catalog.pg_constraint AS con "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "CROSS JOIN LATERAL pg_catalog.unnest(con.confkey) WITH ORDINALITY "
                       "  AS key(attnum, ordinality) "
                       "JOIN pg_catalog.pg_attribute AS a "
                       "  ON a.attrelid = con.confrelid AND a.attnum = key.attnum "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND NOT c.relispartition AND con.contype = 'f' "
                       "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "         con.conname COLLATE \"C\", key.ordinality");
  for (const auto& row : referenced_column_rows) {
    const ConstraintIdentity identity{
        QualifiedName{row[0].as<std::string>(), row[1].as<std::string>()},
        row[2].as<std::string>()};
    const auto found = constraint_positions.find(identity);
    if (found == constraint_positions.end()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL returned a referenced column without its constraint"};
    }
    auto& columns = snapshot.constraints[found->second].referenced_columns;
    if (row[3].as<std::size_t>() != columns.size() + 1U) {
      throw Error{ErrorCode::database,
                  "PostgreSQL referenced constraint columns are not contiguous"};
    }
    columns.push_back(row[4].as<std::string>());
  }

  const auto standalone_index_predicate =
      "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS con "
      "            WHERE con.conindid = i.indexrelid "
      "              AND con.contype IN ('p', 'u', 'x'))";
  const auto index_rows = transaction.exec(
      "SELECT ni.nspname, ci.relname, nt.nspname, ct.relname, am.amname, "
      "       i.indisunique, i.indnullsnotdistinct, "
      "       pg_catalog.pg_get_expr(i.indpred, i.indrelid, false) "
      "FROM pg_catalog.pg_index AS i "
      "JOIN pg_catalog.pg_class AS ci ON ci.oid = i.indexrelid "
      "JOIN pg_catalog.pg_namespace AS ni ON ni.oid = ci.relnamespace "
      "JOIN pg_catalog.pg_class AS ct ON ct.oid = i.indrelid "
      "JOIN pg_catalog.pg_namespace AS nt ON nt.oid = ct.relnamespace "
      "JOIN pg_catalog.pg_am AS am ON am.oid = ci.relam "
      "WHERE nt.nspname IN (" +
      filter + ") AND ct.relkind = 'r' AND NOT ct.relispartition AND ci.relkind = 'i' AND " +
      standalone_index_predicate + " ORDER BY ni.nspname COLLATE \"C\", ci.relname COLLATE \"C\"");

  std::map<QualifiedName, std::size_t> index_positions;
  snapshot.indexes.reserve(static_cast<std::size_t>(index_rows.size()));
  for (const auto& row : index_rows) {
    QualifiedName index_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto position = snapshot.indexes.size();
    if (!index_positions.emplace(index_name, position).second) {
      throw Error{ErrorCode::database, "PostgreSQL returned a duplicate standalone index"};
    }
    std::optional<std::string> predicate;
    if (!row[7].is_null()) {
      predicate = row[7].as<std::string>();
    }
    snapshot.indexes.push_back(Index{
        .name = std::move(index_name),
        .table = QualifiedName{row[2].as<std::string>(), row[3].as<std::string>()},
        .method = row[4].as<std::string>(),
        .unique = row[5].as<bool>(),
        .nulls_not_distinct = row[6].as<bool>(),
        .key_expressions = {},
        .included_columns = {},
        .predicate = std::move(predicate),
    });
  }

  const auto index_component_rows = transaction.exec(
      "SELECT ni.nspname, ci.relname, key.ordinality, i.indnkeyatts, "
      "       pg_catalog.pg_get_indexdef(i.indexrelid, key.ordinality::integer, false), "
      "       a.attname "
      "FROM pg_catalog.pg_index AS i "
      "JOIN pg_catalog.pg_class AS ci ON ci.oid = i.indexrelid "
      "JOIN pg_catalog.pg_namespace AS ni ON ni.oid = ci.relnamespace "
      "JOIN pg_catalog.pg_class AS ct ON ct.oid = i.indrelid "
      "JOIN pg_catalog.pg_namespace AS nt ON nt.oid = ct.relnamespace "
      "CROSS JOIN LATERAL pg_catalog.unnest(i.indkey) WITH ORDINALITY "
      "  AS key(attnum, ordinality) "
      "LEFT JOIN pg_catalog.pg_attribute AS a "
      "  ON a.attrelid = i.indrelid AND a.attnum = key.attnum "
      "WHERE nt.nspname IN (" +
      filter + ") AND ct.relkind = 'r' AND NOT ct.relispartition AND ci.relkind = 'i' AND " +
      standalone_index_predicate +
      " ORDER BY ni.nspname COLLATE \"C\", ci.relname COLLATE \"C\", key.ordinality");
  for (const auto& row : index_component_rows) {
    const QualifiedName index_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto found = index_positions.find(index_name);
    if (found == index_positions.end()) {
      throw Error{ErrorCode::database, "PostgreSQL returned an index component without its index"};
    }
    auto& index = snapshot.indexes[found->second];
    const auto ordinal = row[2].as<std::size_t>();
    const auto key_count = row[3].as<std::size_t>();
    if (ordinal <= key_count) {
      if (ordinal != index.key_expressions.size() + 1U || row[4].is_null()) {
        throw Error{ErrorCode::database, "PostgreSQL index keys are not contiguous"};
      }
      index.key_expressions.push_back(row[4].as<std::string>());
    } else {
      if (ordinal != key_count + index.included_columns.size() + 1U || row[5].is_null()) {
        throw Error{ErrorCode::database, "PostgreSQL included index columns are invalid"};
      }
      index.included_columns.push_back(row[5].as<std::string>());
    }
  }

  const auto policy_rows =
      transaction.exec("SELECT n.nspname, c.relname, p.polname, p.polcmd::text, p.polpermissive, "
                       "       pg_catalog.pg_get_expr(p.polqual, p.polrelid, false), "
                       "       pg_catalog.pg_get_expr(p.polwithcheck, p.polrelid, false) "
                       "FROM pg_catalog.pg_policy AS p "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND NOT c.relispartition "
                       "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "         p.polname COLLATE \"C\"");
  using PolicyIdentity = std::tuple<QualifiedName, std::string>;
  std::map<PolicyIdentity, std::size_t> policy_positions;
  snapshot.policies.reserve(static_cast<std::size_t>(policy_rows.size()));
  for (const auto& row : policy_rows) {
    const QualifiedName table_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto name = row[2].as<std::string>();
    const auto position = snapshot.policies.size();
    if (!policy_positions.emplace(PolicyIdentity{table_name, name}, position).second) {
      throw Error{ErrorCode::database, "PostgreSQL returned a duplicate row-security policy"};
    }
    std::optional<std::string> using_expression;
    if (!row[5].is_null()) {
      using_expression = row[5].as<std::string>();
    }
    std::optional<std::string> check_expression;
    if (!row[6].is_null()) {
      check_expression = row[6].as<std::string>();
    }
    snapshot.policies.push_back(RowSecurityPolicy{
        .table = table_name,
        .name = name,
        .command = parse_policy_command(row[3].as<std::string>()),
        .permissive = row[4].as<bool>(),
        .roles = {},
        .using_expression = std::move(using_expression),
        .check_expression = std::move(check_expression),
    });
  }

  const auto policy_role_rows = transaction.exec(
      "SELECT n.nspname, c.relname, p.polname, role.oid = 0, r.rolname "
      "FROM pg_catalog.pg_policy AS p "
      "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      "CROSS JOIN LATERAL pg_catalog.unnest(p.polroles) AS role(oid) "
      "LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = role.oid "
      "WHERE n.nspname IN (" +
      filter +
      ") AND c.relkind = 'r' AND NOT c.relispartition "
      "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
      "         p.polname COLLATE \"C\", role.oid = 0 DESC, r.rolname COLLATE \"C\"");
  for (const auto& row : policy_role_rows) {
    const PolicyIdentity identity{QualifiedName{row[0].as<std::string>(), row[1].as<std::string>()},
                                  row[2].as<std::string>()};
    const auto found = policy_positions.find(identity);
    if (found == policy_positions.end()) {
      throw Error{ErrorCode::database, "PostgreSQL returned a policy role without its policy"};
    }
    const auto public_role = row[3].as<bool>();
    if (!public_role && row[4].is_null()) {
      throw Error{ErrorCode::database, "PostgreSQL row-security policy has an unknown role"};
    }
    snapshot.policies[found->second].roles.push_back(PolicyRole{
        .public_role = public_role,
        .name = public_role ? std::string{} : row[4].as<std::string>(),
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
                                                "WHERE r.rolname = CURRENT_USER");
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
  hash.add_length_prefixed("dbdiff.postgresql.schema.v2");
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
  for (const auto& constraint : snapshot.constraints) {
    hash_field(hash, "object", "constraint");
    hash_field(hash, "table_schema", constraint.table.schema);
    hash_field(hash, "table_name", constraint.table.name);
    hash_field(hash, "name", constraint.name);
    hash_field(hash, "kind", std::to_string(static_cast<int>(constraint.kind)));
    for (const auto& column : constraint.columns) {
      hash_field(hash, "column", column);
    }
    hash_field(hash, "definition", constraint.definition);
    hash_boolean(hash, "validated", constraint.validated);
    hash_boolean(hash, "enforced", constraint.enforced);
    hash_boolean(hash, "referenced_table_present", constraint.referenced_table.has_value());
    if (const auto referenced_table = constraint.referenced_table; referenced_table.has_value()) {
      const auto& referenced = referenced_table.value();
      hash_field(hash, "referenced_schema", referenced.schema);
      hash_field(hash, "referenced_table", referenced.name);
    }
    for (const auto& column : constraint.referenced_columns) {
      hash_field(hash, "referenced_column", column);
    }
  }
  for (const auto& index : snapshot.indexes) {
    hash_field(hash, "object", "index");
    hash_field(hash, "schema", index.name.schema);
    hash_field(hash, "name", index.name.name);
    hash_field(hash, "table_schema", index.table.schema);
    hash_field(hash, "table_name", index.table.name);
    hash_field(hash, "method", index.method);
    hash_boolean(hash, "unique", index.unique);
    hash_boolean(hash, "nulls_not_distinct", index.nulls_not_distinct);
    for (const auto& expression : index.key_expressions) {
      hash_field(hash, "key_expression", expression);
    }
    for (const auto& column : index.included_columns) {
      hash_field(hash, "included_column", column);
    }
    hash_optional(hash, "predicate", index.predicate);
  }
  for (const auto& policy : snapshot.policies) {
    hash_field(hash, "object", "policy");
    hash_field(hash, "table_schema", policy.table.schema);
    hash_field(hash, "table_name", policy.table.name);
    hash_field(hash, "name", policy.name);
    hash_field(hash, "command", std::to_string(static_cast<int>(policy.command)));
    hash_boolean(hash, "permissive", policy.permissive);
    for (const auto& role : policy.roles) {
      hash_boolean(hash, "public_role", role.public_role);
      hash_field(hash, "role", role.name);
    }
    hash_optional(hash, "using", policy.using_expression);
    hash_optional(hash, "check", policy.check_expression);
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

void validate_source(const std::string_view sql) {
  const auto statements = scan_statements(sql);
  validate_source_statements(sql, statements);
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

  std::map<QualifiedName, const Table*> tables;
  for (const auto& table : snapshot.tables) {
    tables.emplace(table.name, &table);
  }
  const auto find_column = [&tables](const QualifiedName& table_name,
                                     const std::string_view column_name) -> const Column* {
    const auto table = tables.find(table_name);
    if (table == tables.end()) {
      return nullptr;
    }
    const auto column = std::ranges::find(table->second->columns, column_name, &Column::name);
    return column == table->second->columns.end() ? nullptr : &*column;
  };

  std::ranges::sort(snapshot.constraints,
                    [](const TableConstraint& left, const TableConstraint& right) {
                      return std::tie(left.table, left.name) < std::tie(right.table, right.name);
                    });
  for (std::size_t index = 0U; index < snapshot.constraints.size(); ++index) {
    auto& constraint = snapshot.constraints[index];
    if (index != 0U && constraint.table == snapshot.constraints[index - 1U].table &&
        constraint.name == snapshot.constraints[index - 1U].name) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains duplicate table constraints"};
    }
    require_no_nul(constraint.name, "PostgreSQL constraint name");
    require_no_nul(constraint.definition, "PostgreSQL constraint definition");
    if (!tables.contains(constraint.table) || constraint.name.empty() ||
        constraint.definition.empty()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains an invalid table constraint"};
    }
    if (constraint.kind == ConstraintKind::not_null && snapshot.server_version.major < 18) {
      throw Error{ErrorCode::unsupported,
                  "named PostgreSQL NOT NULL constraints require PostgreSQL 18 or newer"};
    }

    std::set<std::string, std::less<>> columns;
    for (const auto& column : constraint.columns) {
      require_no_nul(column, "PostgreSQL constraint column");
      if (column.empty() || find_column(constraint.table, column) == nullptr ||
          !columns.insert(column).second) {
        throw Error{ErrorCode::database,
                    "PostgreSQL schema snapshot contains invalid constraint columns"};
      }
    }
    if ((constraint.kind == ConstraintKind::primary_key ||
         constraint.kind == ConstraintKind::unique ||
         constraint.kind == ConstraintKind::foreign_key) &&
        constraint.columns.empty()) {
      throw Error{ErrorCode::database, "PostgreSQL key constraint does not contain any columns"};
    }
    if (constraint.kind == ConstraintKind::not_null) {
      if (constraint.columns.size() != 1U ||
          !find_column(constraint.table, constraint.columns.front())->not_null) {
        throw Error{ErrorCode::database,
                    "PostgreSQL NOT NULL constraint is inconsistent with its column"};
      }
    }
    if (constraint.kind == ConstraintKind::primary_key &&
        std::ranges::any_of(constraint.columns, [&](const std::string& column) {
          return !find_column(constraint.table, column)->not_null;
        })) {
      throw Error{ErrorCode::database, "PostgreSQL primary-key columns must be marked NOT NULL"};
    }

    if (constraint.kind == ConstraintKind::foreign_key) {
      if (!constraint.referenced_table.has_value() ||
          !tables.contains(*constraint.referenced_table) ||
          constraint.referenced_columns.size() != constraint.columns.size()) {
        throw Error{ErrorCode::unsupported,
                    "PostgreSQL foreign key references an unmanaged or invalid table key"};
      }
      std::set<std::string, std::less<>> referenced_columns;
      for (const auto& column : constraint.referenced_columns) {
        require_no_nul(column, "PostgreSQL referenced constraint column");
        if (column.empty() || find_column(*constraint.referenced_table, column) == nullptr ||
            !referenced_columns.insert(column).second) {
          throw Error{ErrorCode::database,
                      "PostgreSQL schema snapshot contains invalid referenced columns"};
        }
      }
    } else if (constraint.referenced_table.has_value() || !constraint.referenced_columns.empty()) {
      throw Error{ErrorCode::database,
                  "non-foreign-key PostgreSQL constraint has referenced columns"};
    }
  }

  std::ranges::sort(snapshot.indexes, {}, &Index::name);
  std::set<QualifiedName> relation_names;
  for (const auto& table : snapshot.tables) {
    relation_names.insert(table.name);
  }
  for (const auto& index : snapshot.indexes) {
    require_no_nul(index.name.name, "PostgreSQL index name");
    require_no_nul(index.method, "PostgreSQL index method");
    if (!tables.contains(index.table) || index.name.schema != index.table.schema ||
        index.name.name.empty() || index.method.empty() || index.key_expressions.empty() ||
        !relation_names.insert(index.name).second || (index.nulls_not_distinct && !index.unique)) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains an invalid standalone index"};
    }
    for (const auto& expression : index.key_expressions) {
      require_no_nul(expression, "PostgreSQL index key expression");
      if (expression.empty()) {
        throw Error{ErrorCode::database, "PostgreSQL index key expression must not be empty"};
      }
    }
    std::set<std::string, std::less<>> included_columns;
    for (const auto& column : index.included_columns) {
      require_no_nul(column, "PostgreSQL included index column");
      if (column.empty() || find_column(index.table, column) == nullptr ||
          !included_columns.insert(column).second) {
        throw Error{ErrorCode::database,
                    "PostgreSQL schema snapshot contains invalid included index columns"};
      }
    }
    if (index.predicate.has_value()) {
      require_no_nul(*index.predicate, "PostgreSQL index predicate");
      if (index.predicate->empty()) {
        throw Error{ErrorCode::database, "PostgreSQL index predicate must not be empty"};
      }
    }
  }

  std::ranges::sort(snapshot.policies,
                    [](const RowSecurityPolicy& left, const RowSecurityPolicy& right) {
                      return std::tie(left.table, left.name) < std::tie(right.table, right.name);
                    });
  for (std::size_t index = 0U; index < snapshot.policies.size(); ++index) {
    auto& policy = snapshot.policies[index];
    if (index != 0U && policy.table == snapshot.policies[index - 1U].table &&
        policy.name == snapshot.policies[index - 1U].name) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains duplicate row-security policies"};
    }
    require_no_nul(policy.name, "PostgreSQL row-security policy name");
    if (!tables.contains(policy.table) || policy.name.empty() || policy.roles.empty()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains an invalid row-security policy"};
    }
    std::ranges::sort(policy.roles);
    if (std::ranges::adjacent_find(policy.roles) != policy.roles.end()) {
      throw Error{ErrorCode::database, "PostgreSQL row-security policy repeats a role"};
    }
    for (const auto& role : policy.roles) {
      require_no_nul(role.name, "PostgreSQL row-security policy role");
      if (role.public_role != role.name.empty()) {
        throw Error{ErrorCode::database, "PostgreSQL row-security policy has an invalid role"};
      }
      if (!role.public_role) {
        throw Error{ErrorCode::unsupported,
                    "PostgreSQL row-security policies may currently target only the PUBLIC role"};
      }
    }
    if (policy.using_expression.has_value()) {
      require_no_nul(*policy.using_expression, "PostgreSQL policy USING expression");
      if (policy.using_expression->empty()) {
        throw Error{ErrorCode::database, "PostgreSQL policy USING expression must not be empty"};
      }
    }
    if (policy.check_expression.has_value()) {
      require_no_nul(*policy.check_expression, "PostgreSQL policy WITH CHECK expression");
      if (policy.check_expression->empty()) {
        throw Error{ErrorCode::database,
                    "PostgreSQL policy WITH CHECK expression must not be empty"};
      }
    }
    if ((policy.command == PolicyCommand::insert && policy.using_expression.has_value()) ||
        ((policy.command == PolicyCommand::select ||
          policy.command == PolicyCommand::delete_rows) &&
         policy.check_expression.has_value())) {
      throw Error{ErrorCode::database, "PostgreSQL policy expressions do not match its command"};
    }
  }

  std::ranges::sort(snapshot.unsupported_objects);
  for (const auto& object : snapshot.unsupported_objects) {
    require_no_nul(object.kind, "PostgreSQL unsupported catalog kind");
    require_no_nul(object.identity, "PostgreSQL unsupported catalog identity");
    if (object.kind.empty() || object.identity.empty()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL snapshot contains an invalid unsupported catalog object"};
    }
  }
  if (!snapshot.unsupported_objects.empty()) {
    const auto& object = snapshot.unsupported_objects.front();
    throw Error{ErrorCode::unsupported, "unsupported PostgreSQL " + object.kind + " " +
                                            object.identity + " exists in a managed schema"};
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

[[nodiscard]] std::string render_column_definition(const Column& column,
                                                   const bool include_not_null = true) {
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
  if (column.not_null && include_not_null) {
    sql += " NOT NULL";
  }
  return sql;
}

[[nodiscard]] bool has_not_null_constraint(const std::vector<TableConstraint>& constraints,
                                           const QualifiedName& table,
                                           const std::string_view column) {
  return std::ranges::any_of(constraints, [&](const TableConstraint& constraint) {
    return constraint.table == table && constraint.kind == ConstraintKind::not_null &&
           constraint.columns.size() == 1U && constraint.columns.front() == column;
  });
}

[[nodiscard]] std::string render_create_table(const Table& table,
                                              const std::vector<TableConstraint>& constraints) {
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
    sql << render_column_definition(
        table.columns[index],
        !has_not_null_constraint(constraints, table.name, table.columns[index].name));
  }
  sql << ");";
  return sql.str();
}

[[nodiscard]] std::string constraint_identity(const TableConstraint& constraint) {
  return qualified_identity(constraint.table) + ".constraint." + constraint.name;
}

[[nodiscard]] std::string render_add_constraint(const TableConstraint& constraint) {
  return "ALTER TABLE " + qualified_sql(constraint.table) + " ADD CONSTRAINT " +
         quote_identifier(constraint.name) + " " + constraint.definition + ";";
}

[[nodiscard]] std::string render_drop_constraint(const TableConstraint& constraint) {
  return "ALTER TABLE " + qualified_sql(constraint.table) + " DROP CONSTRAINT " +
         quote_identifier(constraint.name) + " RESTRICT;";
}

[[nodiscard]] std::string render_create_index(const Index& index) {
  std::ostringstream sql;
  sql << "CREATE ";
  if (index.unique) {
    sql << "UNIQUE ";
  }
  sql << "INDEX " << quote_identifier(index.name.name) << " ON " << qualified_sql(index.table)
      << " USING " << quote_identifier(index.method) << " (";
  for (std::size_t position = 0U; position < index.key_expressions.size(); ++position) {
    if (position != 0U) {
      sql << ", ";
    }
    sql << index.key_expressions[position];
  }
  sql << ')';
  if (!index.included_columns.empty()) {
    sql << " INCLUDE (";
    for (std::size_t position = 0U; position < index.included_columns.size(); ++position) {
      if (position != 0U) {
        sql << ", ";
      }
      sql << quote_identifier(index.included_columns[position]);
    }
    sql << ')';
  }
  if (index.nulls_not_distinct) {
    sql << " NULLS NOT DISTINCT";
  }
  if (index.predicate.has_value()) {
    sql << " WHERE " << *index.predicate;
  }
  sql << ';';
  return sql.str();
}

[[nodiscard]] std::string index_identity(const Index& index) {
  return qualified_identity(index.name);
}

[[nodiscard]] std::string policy_command_sql(const PolicyCommand command) {
  switch (command) {
  case PolicyCommand::all:
    return "ALL";
  case PolicyCommand::select:
    return "SELECT";
  case PolicyCommand::insert:
    return "INSERT";
  case PolicyCommand::update:
    return "UPDATE";
  case PolicyCommand::delete_rows:
    return "DELETE";
  }
  throw Error{ErrorCode::database, "invalid PostgreSQL policy command"};
}

[[nodiscard]] std::string policy_identity(const RowSecurityPolicy& policy) {
  return qualified_identity(policy.table) + ".policy." + policy.name;
}

[[nodiscard]] std::string render_create_policy(const RowSecurityPolicy& policy) {
  std::ostringstream sql;
  sql << "CREATE POLICY " << quote_identifier(policy.name) << " ON " << qualified_sql(policy.table)
      << " AS " << (policy.permissive ? "PERMISSIVE" : "RESTRICTIVE") << " FOR "
      << policy_command_sql(policy.command) << " TO ";
  for (std::size_t position = 0U; position < policy.roles.size(); ++position) {
    if (position != 0U) {
      sql << ", ";
    }
    sql << (policy.roles[position].public_role ? "PUBLIC"
                                               : quote_identifier(policy.roles[position].name));
  }
  if (policy.using_expression.has_value()) {
    sql << " USING (" << *policy.using_expression << ')';
  }
  if (policy.check_expression.has_value()) {
    sql << " WITH CHECK (" << *policy.check_expression << ')';
  }
  sql << ';';
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

using TableChildIdentity = std::pair<QualifiedName, std::string>;

[[nodiscard]] std::map<TableChildIdentity, const TableConstraint*>
constraint_index(const std::vector<TableConstraint>& constraints) {
  std::map<TableChildIdentity, const TableConstraint*> result;
  for (const auto& constraint : constraints) {
    result.emplace(TableChildIdentity{constraint.table, constraint.name}, &constraint);
  }
  return result;
}

[[nodiscard]] std::map<QualifiedName, const Index*> index_index(const std::vector<Index>& indexes) {
  std::map<QualifiedName, const Index*> result;
  for (const auto& index : indexes) {
    result.emplace(index.name, &index);
  }
  return result;
}

[[nodiscard]] std::map<TableChildIdentity, const RowSecurityPolicy*>
policy_index(const std::vector<RowSecurityPolicy>& policies) {
  std::map<TableChildIdentity, const RowSecurityPolicy*> result;
  for (const auto& policy : policies) {
    result.emplace(TableChildIdentity{policy.table, policy.name}, &policy);
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

[[nodiscard]] std::vector<std::string>
plan_row_security(MigrationPlan& migration_plan, const Table& from, const Table& to,
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
    dependencies = {add_operation(migration_plan, action, identity, sql,
                                  HazardSet{Hazard::write_lock}, std::move(dependencies))};
  }
  return dependencies;
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
  const auto from_constraints = constraint_index(from.constraints);
  const auto to_constraints = constraint_index(to.constraints);
  const auto from_indexes = index_index(from.indexes);
  const auto to_indexes = index_index(to.indexes);
  const auto from_policies = policy_index(from.policies);
  const auto to_policies = policy_index(to.policies);

  const auto append_dependencies = [](std::vector<std::string>& dependencies,
                                      const std::vector<std::string>& additions) {
    for (const auto& addition : additions) {
      if (std::ranges::find(dependencies, addition) == dependencies.end()) {
        dependencies.push_back(addition);
      }
    }
  };
  const auto append_dependency = [](std::vector<std::string>& dependencies,
                                    const std::string& addition) {
    if (std::ranges::find(dependencies, addition) == dependencies.end()) {
      dependencies.push_back(addition);
    }
  };

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

  std::set<QualifiedName> relational_blocked_tables;
  for (const auto& [name, source_table] : from_tables) {
    const auto target = to_tables.find(name);
    if (target == to_tables.end()) {
      continue;
    }
    const auto source_columns = column_index(source_table->columns);
    const auto target_columns = column_index(target->second->columns);
    for (const auto& [source_name, source_column] : source_columns) {
      if (target_columns.contains(source_name)) {
        continue;
      }
      if (std::ranges::any_of(target_columns, [&](const auto& candidate) {
            return !source_columns.contains(candidate.first) &&
                   same_column_shape(*source_column, *candidate.second);
          })) {
        relational_blocked_tables.insert(name);
      }
    }
  }

  const auto blocked_source_table = [&](const QualifiedName& table) {
    return possible_table_renames_from.contains(table) || relational_blocked_tables.contains(table);
  };
  const auto blocked_target_table = [&](const QualifiedName& table) {
    return possible_table_renames_to.contains(table) || relational_blocked_tables.contains(table);
  };

  std::set<QualifiedName> changed_unique_tables;
  for (const auto& [identity, constraint] : from_constraints) {
    if (blocked_source_table(constraint->table) ||
        (constraint->kind != ConstraintKind::primary_key &&
         constraint->kind != ConstraintKind::unique)) {
      continue;
    }
    const auto target = to_constraints.find(identity);
    if (target == to_constraints.end() || *target->second != *constraint) {
      changed_unique_tables.insert(constraint->table);
    }
  }
  for (const auto& [identity, constraint] : to_constraints) {
    if (blocked_target_table(constraint->table) ||
        (constraint->kind != ConstraintKind::primary_key &&
         constraint->kind != ConstraintKind::unique)) {
      continue;
    }
    const auto source = from_constraints.find(identity);
    if (source == from_constraints.end() || *source->second != *constraint) {
      changed_unique_tables.insert(constraint->table);
    }
  }
  for (const auto& [identity, index] : from_indexes) {
    if (!index->unique || blocked_source_table(index->table)) {
      continue;
    }
    const auto target = to_indexes.find(identity);
    if (target == to_indexes.end() || *target->second != *index) {
      changed_unique_tables.insert(index->table);
    }
  }
  for (const auto& [identity, index] : to_indexes) {
    if (!index->unique || blocked_target_table(index->table)) {
      continue;
    }
    const auto source = from_indexes.find(identity);
    if (source == from_indexes.end() || *source->second != *index) {
      changed_unique_tables.insert(index->table);
    }
  }

  const auto constraint_requires_drop = [&](const TableChildIdentity& identity,
                                            const TableConstraint& constraint) {
    if (blocked_source_table(constraint.table)) {
      return false;
    }
    const auto target = to_constraints.find(identity);
    return target == to_constraints.end() || *target->second != constraint ||
           (constraint.kind == ConstraintKind::foreign_key &&
            constraint.referenced_table.has_value() &&
            changed_unique_tables.contains(*constraint.referenced_table));
  };
  const auto constraint_requires_create = [&](const TableChildIdentity& identity,
                                              const TableConstraint& constraint) {
    if (blocked_target_table(constraint.table)) {
      return false;
    }
    const auto source = from_constraints.find(identity);
    return source == from_constraints.end() || *source->second != constraint ||
           (constraint.kind == ConstraintKind::foreign_key &&
            constraint.referenced_table.has_value() &&
            changed_unique_tables.contains(*constraint.referenced_table));
  };

  std::map<QualifiedName, std::vector<std::string>> structural_prerequisites;
  std::map<QualifiedName, std::vector<std::string>> inbound_foreign_key_drops;

  for (const auto& [identity, policy] : from_policies) {
    if (blocked_source_table(policy->table)) {
      continue;
    }
    const auto target = to_policies.find(identity);
    if (target == to_policies.end() || *target->second != *policy) {
      const auto id = add_operation(migration_plan, "policy.drop", policy_identity(*policy),
                                    "DROP POLICY " + quote_identifier(policy->name) + " ON " +
                                        qualified_sql(policy->table) + ";",
                                    HazardSet{Hazard::write_lock});
      structural_prerequisites[policy->table].push_back(id);
    }
  }

  for (const auto& [identity, constraint] : from_constraints) {
    if (constraint->kind != ConstraintKind::foreign_key ||
        !constraint_requires_drop(identity, *constraint)) {
      continue;
    }
    const auto id =
        add_operation(migration_plan, "constraint.drop", constraint_identity(*constraint),
                      render_drop_constraint(*constraint), HazardSet{Hazard::write_lock});
    append_dependency(structural_prerequisites[constraint->table], id);
    if (constraint->referenced_table.has_value()) {
      append_dependency(structural_prerequisites[*constraint->referenced_table], id);
      append_dependency(inbound_foreign_key_drops[*constraint->referenced_table], id);
    }
  }

  std::map<QualifiedName, std::vector<std::string>> non_not_null_constraint_drops;
  for (const auto& [identity, constraint] : from_constraints) {
    if (constraint->kind == ConstraintKind::foreign_key ||
        constraint->kind == ConstraintKind::not_null ||
        !constraint_requires_drop(identity, *constraint)) {
      continue;
    }
    std::vector<std::string> dependencies;
    if (constraint->kind == ConstraintKind::primary_key ||
        constraint->kind == ConstraintKind::unique) {
      append_dependencies(dependencies, inbound_foreign_key_drops[constraint->table]);
    }
    const auto id =
        add_operation(migration_plan, "constraint.drop", constraint_identity(*constraint),
                      render_drop_constraint(*constraint), HazardSet{Hazard::write_lock},
                      std::move(dependencies));
    structural_prerequisites[constraint->table].push_back(id);
    non_not_null_constraint_drops[constraint->table].push_back(id);
  }

  for (const auto& [identity, constraint] : from_constraints) {
    if (constraint->kind != ConstraintKind::not_null ||
        !constraint_requires_drop(identity, *constraint)) {
      continue;
    }
    auto dependencies = non_not_null_constraint_drops[constraint->table];
    const auto id =
        add_operation(migration_plan, "constraint.drop", constraint_identity(*constraint),
                      render_drop_constraint(*constraint), HazardSet{Hazard::write_lock},
                      std::move(dependencies));
    structural_prerequisites[constraint->table].push_back(id);
  }

  for (const auto& [identity, index] : from_indexes) {
    if (blocked_source_table(index->table)) {
      continue;
    }
    const auto target = to_indexes.find(identity);
    if (target == to_indexes.end() || *target->second != *index) {
      std::vector<std::string> dependencies;
      if (index->unique) {
        append_dependencies(dependencies, inbound_foreign_key_drops[index->table]);
      }
      const auto id = add_operation(migration_plan, "index.drop", index_identity(*index),
                                    "DROP INDEX " + qualified_sql(index->name) + " RESTRICT;",
                                    HazardSet{Hazard::write_lock}, std::move(dependencies));
      structural_prerequisites[index->table].push_back(id);
    }
  }

  std::map<std::string, std::vector<std::string>, std::less<>> dropped_table_operations;
  std::map<QualifiedName, std::vector<std::string>> table_tail_operations;
  for (const auto& [name, table] : to_tables) {
    if (from_tables.contains(name) || possible_table_renames_to.contains(name)) {
      continue;
    }
    std::vector<std::string> dependencies;
    if (const auto schema = schema_create_operations.find(name.schema);
        schema != schema_create_operations.end()) {
      dependencies.push_back(schema->second);
    }
    const auto create_id =
        add_operation(migration_plan, "table.create", qualified_identity(name),
                      render_create_table(*table, to.constraints), {}, std::move(dependencies));
    const Table empty_table{
        .name = name,
        .persistence = table->persistence,
        .row_security = false,
        .force_row_security = false,
        .columns = table->columns,
    };
    table_tail_operations[name] =
        plan_row_security(migration_plan, empty_table, *table, {create_id});
  }

  for (const auto& [name, from_table] : from_tables) {
    const auto target = to_tables.find(name);
    if (target == to_tables.end()) {
      if (!possible_table_renames_from.contains(name)) {
        auto dependencies = structural_prerequisites[name];
        const auto id = add_operation(migration_plan, "table.drop", qualified_identity(name),
                                      "DROP TABLE " + qualified_sql(name) + " RESTRICT;",
                                      HazardSet{Hazard::data_loss, Hazard::write_lock},
                                      std::move(dependencies));
        dropped_table_operations[name.schema].push_back(id);
      }
      continue;
    }

    const auto* to_table = target->second;
    auto table_dependencies = structural_prerequisites[name];
    if (from_table->persistence != to_table->persistence) {
      const auto sql =
          "ALTER TABLE " + qualified_sql(name) +
          (to_table->persistence == TablePersistence::unlogged ? " SET UNLOGGED;" : " SET LOGGED;");
      table_dependencies = {add_operation(
          migration_plan, "table.persistence", qualified_identity(name), sql,
          HazardSet{Hazard::table_rewrite, Hazard::write_lock}, std::move(table_dependencies))};
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
          relational_blocked_tables.insert(name);
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
      relational_blocked_tables.insert(name);
      add_diagnostic(migration_plan, "column reordering on " + qualified_sql(name) +
                                         " cannot be represented safely by PostgreSQL ALTER TABLE");
    }
    for (const auto& inserted : inserted_columns) {
      relational_blocked_tables.insert(name);
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
        relational_blocked_tables.insert(name);
        continue;
      }
      HazardSet hazards{Hazard::write_lock};
      if (column.default_expression.has_value() || column.generated != GeneratedStorage::none) {
        hazards.insert(Hazard::table_rewrite);
      }
      table_dependencies = {add_operation(
          migration_plan, "column.add", column_identity(name, column.name),
          "ALTER TABLE " + qualified_sql(name) + " ADD COLUMN " +
              render_column_definition(
                  column, !has_not_null_constraint(to.constraints, name, column.name)) +
              ";",
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
        relational_blocked_tables.insert(name);
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
        const auto target_constraint_sets_not_null =
            std::ranges::any_of(to.constraints, [&](const TableConstraint& constraint) {
              return constraint.table == name &&
                     (constraint.kind == ConstraintKind::primary_key ||
                      constraint.kind == ConstraintKind::not_null) &&
                     std::ranges::find(constraint.columns, from_column.name) !=
                         constraint.columns.end();
            });
        const auto source_named_not_null_dropped =
            !to_column.not_null &&
            has_not_null_constraint(from.constraints, name, from_column.name);
        if (target_constraint_sets_not_null || source_named_not_null_dropped) {
          continue;
        }
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
    table_tail_operations[name] =
        plan_row_security(migration_plan, *from_table, *to_table, std::move(table_dependencies));
  }

  std::map<QualifiedName, std::vector<std::string>> key_create_operations;
  const auto constraint_hazards = [](const TableConstraint& constraint) {
    HazardSet hazards{Hazard::write_lock};
    if (constraint.validated && constraint.enforced) {
      hazards.insert(Hazard::constraint_scan);
    }
    return hazards;
  };

  std::map<QualifiedName, std::vector<std::string>> not_null_create_operations;
  for (const auto& [identity, constraint] : to_constraints) {
    if (constraint->kind != ConstraintKind::not_null ||
        !constraint_requires_create(identity, *constraint) ||
        relational_blocked_tables.contains(constraint->table)) {
      continue;
    }
    auto dependencies = table_tail_operations[constraint->table];
    const auto id =
        add_operation(migration_plan, "constraint.create", constraint_identity(*constraint),
                      render_add_constraint(*constraint), constraint_hazards(*constraint),
                      std::move(dependencies));
    not_null_create_operations[constraint->table].push_back(id);
  }

  for (const auto& [identity, constraint] : to_constraints) {
    if (constraint->kind == ConstraintKind::foreign_key ||
        constraint->kind == ConstraintKind::not_null ||
        !constraint_requires_create(identity, *constraint) ||
        relational_blocked_tables.contains(constraint->table)) {
      continue;
    }
    auto dependencies = table_tail_operations[constraint->table];
    append_dependencies(dependencies, not_null_create_operations[constraint->table]);
    const auto id =
        add_operation(migration_plan, "constraint.create", constraint_identity(*constraint),
                      render_add_constraint(*constraint), constraint_hazards(*constraint),
                      std::move(dependencies));
    if (constraint->kind == ConstraintKind::primary_key ||
        constraint->kind == ConstraintKind::unique) {
      key_create_operations[constraint->table].push_back(id);
    }
  }

  for (const auto& [identity, index] : to_indexes) {
    if (blocked_target_table(index->table) || relational_blocked_tables.contains(index->table)) {
      continue;
    }
    const auto source = from_indexes.find(identity);
    if (source != from_indexes.end() && *source->second == *index) {
      continue;
    }
    auto dependencies = table_tail_operations[index->table];
    const auto id = add_operation(migration_plan, "index.create", index_identity(*index),
                                  render_create_index(*index), HazardSet{Hazard::write_lock},
                                  std::move(dependencies));
    if (index->unique) {
      key_create_operations[index->table].push_back(id);
    }
  }

  for (const auto& [identity, constraint] : to_constraints) {
    if (constraint->kind != ConstraintKind::foreign_key ||
        !constraint_requires_create(identity, *constraint) ||
        relational_blocked_tables.contains(constraint->table)) {
      continue;
    }
    auto dependencies = table_tail_operations[constraint->table];
    if (constraint->referenced_table.has_value()) {
      append_dependencies(dependencies, table_tail_operations[*constraint->referenced_table]);
      append_dependencies(dependencies, key_create_operations[*constraint->referenced_table]);
    }
    static_cast<void>(add_operation(migration_plan, "constraint.create",
                                    constraint_identity(*constraint),
                                    render_add_constraint(*constraint),
                                    constraint_hazards(*constraint), std::move(dependencies)));
  }

  for (const auto& [identity, policy] : to_policies) {
    if (blocked_target_table(policy->table) || relational_blocked_tables.contains(policy->table)) {
      continue;
    }
    const auto source = from_policies.find(identity);
    if (source != from_policies.end() && *source->second == *policy) {
      continue;
    }
    static_cast<void>(add_operation(migration_plan, "policy.create", policy_identity(*policy),
                                    render_create_policy(*policy), HazardSet{Hazard::write_lock},
                                    table_tail_operations[policy->table]));
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
