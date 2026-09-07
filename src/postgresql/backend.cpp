#include "internal.hpp"

namespace dbdiff::postgresql {

BackendKind kind() noexcept { return BackendKind::postgresql; }

SchemaSnapshot introspect_database(const ConnectionLocator& locator,
                                   const std::vector<std::string>& managed_schemas) {
  try {
    const auto connection_string = locator.connection_string();
    pqxx::connection connection{connection_string.c_str()};
    return detail::introspect_connection(connection, managed_schemas);
  } catch (const Error&) {
    throw;
  } catch (const std::exception&) {
    detail::throw_connection_error("schema introspection");
  }
}

} // namespace dbdiff::postgresql
