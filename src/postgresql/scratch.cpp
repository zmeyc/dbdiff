#include "internal.hpp"

namespace dbdiff::postgresql {
namespace detail {

constexpr std::string_view scratch_prefix = "dbdiff_";
constexpr std::size_t scratch_token_size = 32;

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

[[nodiscard]] std::uint64_t provisioning_owner(pqxx::transaction_base& transaction) {
  const auto row = execute_one_row(transaction, "SELECT r.oid::bigint "
                                                "FROM pg_catalog.pg_roles AS r "
                                                "WHERE r.rolname = CURRENT_USER");
  return row[0].as<std::uint64_t>();
}

void drop_scratch_database(const ConnectionLocator& provisioning_locator,
                           const ScratchIdentity& identity, const std::uint64_t owner_oid,
                           const ConnectionSettings& settings) {
  const auto connection_string = provisioning_locator.connection_string();
  pqxx::connection connection{connection_string.c_str()};
  pqxx::nontransaction transaction{connection};
  configure_connection(transaction, settings);

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
                                        const std::uint64_t owner_oid,
                                        const ConnectionSettings& settings) noexcept {
  try {
    drop_scratch_database(provisioning_locator, identity, owner_oid, settings);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

} // namespace detail

using namespace detail;

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

struct ScratchDatabase::Impl {
  ConnectionLocator provisioning_locator;
  ScratchIdentity identity;
  ServerVersion version;
  std::uint64_t owner_oid{};
  ConnectionSettings settings;
  std::unique_ptr<Database> database;
  bool closed{false};

  Impl(ConnectionLocator provisioning_locator_value, ScratchIdentity identity_value,
       const ServerVersion server_version, const std::uint64_t provisioning_owner_oid,
       const ConnectionSettings connection_settings, Database database_session)
      : provisioning_locator{std::move(provisioning_locator_value)},
        identity{std::move(identity_value)}, version{server_version},
        owner_oid{provisioning_owner_oid}, settings{connection_settings},
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
    drop_scratch_database(provisioning_locator, identity, owner_oid, settings);
    closed = true;
  }
};

ScratchDatabase::ScratchDatabase(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

ScratchDatabase ScratchDatabase::create(const ConnectionLocator& provisioning_locator,
                                        const ConnectionSettings settings) {
  const auto identity = ScratchIdentity::generate();
  std::uint64_t owner_oid{};
  ServerVersion version{};
  bool database_created = false;

  try {
    const auto connection_string = provisioning_locator.connection_string();
    pqxx::connection provisioning_connection{connection_string.c_str()};
    {
      pqxx::nontransaction transaction{provisioning_connection};
      configure_connection(transaction, settings);
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
    auto database = Database::open(database_locator, settings);
    return ScratchDatabase{std::make_unique<Impl>(provisioning_locator, identity, version,
                                                  owner_oid, settings, std::move(database))};
  } catch (const Error& error) {
    if (database_created &&
        !try_creation_cleanup(provisioning_locator, identity, owner_oid, settings)) {
      throw Error{error.code(),
                  std::string{error.what()} + "; automatic scratch cleanup also failed"};
    }
    throw;
  } catch (const std::exception&) {
    if (database_created &&
        !try_creation_cleanup(provisioning_locator, identity, owner_oid, settings)) {
      throw Error{ErrorCode::database,
                  "PostgreSQL scratch database creation and automatic cleanup failed"};
    }
    throw_connection_error("scratch database creation");
  }
}

ScratchDatabase ScratchDatabase::create(const std::string_view provisioning_locator,
                                        const ConnectionSettings settings) {
  return create(ConnectionLocator::parse(provisioning_locator), settings);
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
