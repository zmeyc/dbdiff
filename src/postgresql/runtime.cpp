#include "internal.hpp"

namespace dbdiff::postgresql {
namespace detail {

constexpr int minimum_server_version = 150000;
constexpr int maximum_server_version = 190000;

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

void configure_connection(pqxx::transaction_base& transaction, const ConnectionSettings& settings) {
  constexpr auto maximum_timeout = std::numeric_limits<int>::max();
  if (settings.lock_timeout.count() <= 0 || settings.lock_timeout.count() > maximum_timeout ||
      settings.statement_timeout.count() <= 0 ||
      settings.statement_timeout.count() > maximum_timeout) {
    throw Error{ErrorCode::configuration, "PostgreSQL connection timeouts are outside range"};
  }
  execute_no_rows(transaction, "SET standard_conforming_strings TO on");
  execute_no_rows(transaction,
                  "SET statement_timeout TO " +
                      quote_literal(std::to_string(settings.statement_timeout.count()) + "ms"));
  execute_no_rows(transaction,
                  "SET lock_timeout TO " +
                      quote_literal(std::to_string(settings.lock_timeout.count()) + "ms"));
}

[[nodiscard]] ServerVersion connection_server_version(pqxx::transaction_base& transaction) {
  const auto row =
      execute_one_row(transaction, "SELECT pg_catalog.current_setting('server_version_num')");
  return parse_server_version(row[0].as<std::string>());
}

[[noreturn]] void throw_connection_error(const std::string_view operation) {
  throw Error{ErrorCode::database, "PostgreSQL " + std::string{operation} + " failed"};
}

SessionAdvisoryLock::SessionAdvisoryLock(pqxx::nontransaction& transaction)
    : transaction_{transaction} {
  static_cast<void>(execute_one_row(transaction_, advisory_lock_sql));
}

SessionAdvisoryLock::~SessionAdvisoryLock() {
  try {
    static_cast<void>(execute_one_row(transaction_, advisory_unlock_sql));
  } catch (const std::exception& error) {
    // Cleanup must preserve the original failure; closing the session also releases its locks.
    static_cast<void>(error);
  }
}

} // namespace detail

using namespace detail;

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

struct LifecycleLock::Impl {
  explicit Impl(std::shared_ptr<pqxx::connection> database_connection)
      : connection{std::move(database_connection)} {}

  ~Impl() {
    if (!connection || !connection->is_open()) {
      return;
    }
    try {
      pqxx::nontransaction transaction{*connection};
      const auto row = execute_one_row(transaction, advisory_unlock_sql);
      static_cast<void>(row);
    } catch (const std::exception& error) {
      static_cast<void>(error);
    }
  }

  std::shared_ptr<pqxx::connection> connection;
};

LifecycleLock::LifecycleLock(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

LifecycleLock::LifecycleLock(LifecycleLock&&) noexcept = default;

LifecycleLock& LifecycleLock::operator=(LifecycleLock&&) noexcept = default;

LifecycleLock::~LifecycleLock() = default;

Database::Database(std::unique_ptr<Impl> implementation)
    : implementation_{std::move(implementation)} {}

Database::~Database() = default;

Database::Database(Database&&) noexcept = default;

Database& Database::operator=(Database&&) noexcept = default;

Database Database::open(const ConnectionLocator& locator, const ConnectionSettings settings) {
  try {
    const auto connection_string = locator.connection_string();
    auto connection = std::make_shared<pqxx::connection>(connection_string.c_str());
    pqxx::nontransaction transaction{*connection};
    configure_connection(transaction, settings);
    const auto version = connection_server_version(transaction);
    return Database{std::make_unique<Impl>(std::move(connection), version, settings)};
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw_connection_error("connection");
  }
}

Database Database::open(const std::string_view locator, const ConnectionSettings settings) {
  return open(ConnectionLocator::parse(locator), settings);
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

LifecycleLock Database::acquire_lifecycle_lock() & {
  if (!is_open()) {
    throw Error{ErrorCode::database, "PostgreSQL database session is not open"};
  }
  try {
    pqxx::nontransaction transaction{*implementation_->connection};
    const auto deadline = std::chrono::steady_clock::now() + implementation_->settings.lock_timeout;
    constexpr auto poll_interval = std::chrono::milliseconds{10};
    while (true) {
      const auto row = execute_one_row(transaction, advisory_try_lock_sql);
      if (!row[0].is_null() && row[0].as<bool>()) {
        return LifecycleLock{std::make_unique<LifecycleLock::Impl>(implementation_->connection)};
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        throw Error{ErrorCode::database, "PostgreSQL lifecycle lock acquisition timed out"};
      }
      const auto poll =
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(poll_interval);
      std::this_thread::sleep_for(std::min(deadline - now, poll));
    }
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    throw Error{ErrorCode::database, "PostgreSQL lifecycle lock acquisition failed"};
  }
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

} // namespace dbdiff::postgresql
