#include "dbdiff/backend.hpp"

namespace dbdiff::postgresql {

BackendKind kind() noexcept { return BackendKind::postgresql; }

} // namespace dbdiff::postgresql
