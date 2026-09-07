#pragma once
#include "connection.hpp"
namespace dbdiff::sqlite {
struct Database::Impl {
  Impl(const std::string& locator, const OpenMode mode, const ConnectionSettings& settings)
      : settings{settings}, progress{settings.statement_timeout} {
    const auto result =
        sqlite3_open_v2(locator.c_str(), &handle, detail::open_flags(mode), nullptr);
    if (result != SQLITE_OK) {
      const auto message = detail::sqlite_error_message(handle, result, "opening database");
      if (handle != nullptr) {
        static_cast<void>(sqlite3_close_v2(handle));
        handle = nullptr;
      }
      throw Error{ErrorCode::database, message};
    }
    try {
      detail::configure_database(handle, progress, settings);
    } catch (...) {
      static_cast<void>(sqlite3_close_v2(handle));
      handle = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (handle != nullptr) {
      static_cast<void>(sqlite3_close_v2(handle));
    }
  }

  sqlite3* handle{nullptr};
  ConnectionSettings settings;
  detail::ProgressState progress;
};

} // namespace dbdiff::sqlite
