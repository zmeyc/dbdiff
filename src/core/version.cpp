#include "dbdiff/version.hpp"
#include "dbdiff/backend.hpp"

#include <string_view>

namespace dbdiff {

std::string_view version() noexcept { return "0.1.0"; }

std::string_view backend_name(const BackendKind backend) noexcept {
  switch (backend) {
  case BackendKind::postgresql:
    return "postgresql";
  case BackendKind::sqlite:
    return "sqlite";
  }
  return "unknown";
}

} // namespace dbdiff
