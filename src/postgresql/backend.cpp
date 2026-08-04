#include "dbdiff/postgresql.hpp"

#include "dbdiff/error.hpp"

#include <libpq-fe.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

} // namespace

BackendKind kind() noexcept { return BackendKind::postgresql; }

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
    for (const auto& column : table.columns) {
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
      previous_position = column.position;
    }
  }
  return snapshot;
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

struct ScratchDatabase::Impl {
  ConnectionLocator provisioning_locator;
  ScratchIdentity identity;
  ServerVersion version;
  std::uint64_t owner_oid{};
  std::unique_ptr<pqxx::connection> connection;
  bool closed{false};

  Impl(ConnectionLocator provisioning_locator_value, ScratchIdentity identity_value,
       const ServerVersion server_version, const std::uint64_t provisioning_owner_oid,
       std::unique_ptr<pqxx::connection> database_connection)
      : provisioning_locator{std::move(provisioning_locator_value)},
        identity{std::move(identity_value)}, version{server_version},
        owner_oid{provisioning_owner_oid}, connection{std::move(database_connection)} {}

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
    connection.reset();
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

    auto database_locator = provisioning_locator.with_database(identity.name.value());
    const auto database_connection_string = database_locator.connection_string();
    auto database_connection =
        std::make_unique<pqxx::connection>(database_connection_string.c_str());
    return ScratchDatabase{std::make_unique<Impl>(provisioning_locator, identity, version,
                                                  owner_oid, std::move(database_connection))};
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
  return implementation_ && !implementation_->closed && implementation_->connection;
}

SchemaSnapshot ScratchDatabase::introspect(const std::vector<std::string>& managed_schemas) const {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL scratch database is closed"};
  }
  try {
    return introspect_connection(*implementation_->connection, managed_schemas);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw_connection_error("scratch schema introspection");
  }
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
