#include "dbdiff/backend.hpp"

namespace dbdiff::sqlite {

BackendKind kind() noexcept { return BackendKind::sqlite; }

} // namespace dbdiff::sqlite
