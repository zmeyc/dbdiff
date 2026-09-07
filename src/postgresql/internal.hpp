#pragma once

#include "dbdiff/postgresql.hpp"

#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"
#include "dbdiff/migration.hpp"

#include "pqxx.hpp"
#include "sql.hpp"
#include <libpq-fe.h>
#include <openssl/rand.h>

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
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace dbdiff::postgresql {

struct Database::Impl {
  std::shared_ptr<pqxx::connection> connection;
  ServerVersion version;
  ConnectionSettings settings;

  Impl(std::shared_ptr<pqxx::connection> database_connection, const ServerVersion server_version,
       const ConnectionSettings connection_settings)
      : connection{std::move(database_connection)}, version{server_version},
        settings{connection_settings} {}
};

namespace detail {

inline constexpr std::string_view metadata_schema = "_dbdiff";
inline constexpr std::string_view advisory_lock_sql =
    "SELECT pg_catalog.pg_advisory_lock(168430090, 1145194822)";
inline constexpr std::string_view advisory_try_lock_sql =
    "SELECT pg_catalog.pg_try_advisory_lock(168430090, 1145194822)";
inline constexpr std::string_view advisory_unlock_sql =
    "SELECT pg_catalog.pg_advisory_unlock(168430090, 1145194822)";

bool contains_nul(std::string_view value) noexcept;
void require_no_nul(std::string_view value, std::string_view description);
void execute_no_rows(pqxx::transaction_base& transaction, std::string_view statement);
pqxx::row execute_one_row(pqxx::transaction_base& transaction, std::string_view statement);
pqxx::result execute_one_parameter(pqxx::transaction_base& transaction, std::string_view statement,
                                   const std::string& parameter);
void configure_connection(pqxx::transaction_base& transaction, const ConnectionSettings& settings);
ServerVersion connection_server_version(pqxx::transaction_base& transaction);
[[noreturn]] void throw_connection_error(std::string_view operation);
SchemaSnapshot introspect_connection(pqxx::connection& connection,
                                     const std::vector<std::string>& requested_schemas);
void validate_source_statements(std::string_view sql, const std::vector<StatementSpan>& statements);
void execute_plain_unit(pqxx::nontransaction& transaction, const ParsedScript& parsed,
                        const ExecutionUnit& unit);
void execute_transaction_unit(pqxx::nontransaction& transaction, const ParsedScript& parsed,
                              const ExecutionUnit& unit,
                              const std::function<void()>& before_commit);
bool replayable_session_unit(const ParsedScript& parsed, const ExecutionUnit& unit);
void validate_resumable_session_units(const ParsedScript& parsed);
void execute_unit_prefix(pqxx::nontransaction& transaction, const ParsedScript& parsed,
                         std::size_t completed_unit_count);

class SessionAdvisoryLock final {
public:
  explicit SessionAdvisoryLock(pqxx::nontransaction& transaction);
  SessionAdvisoryLock(const SessionAdvisoryLock&) = delete;
  SessionAdvisoryLock& operator=(const SessionAdvisoryLock&) = delete;
  ~SessionAdvisoryLock();

private:
  pqxx::nontransaction& transaction_;
};

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

} // namespace detail
} // namespace dbdiff::postgresql
