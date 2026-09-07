#pragma once
#include "dbdiff/sqlite.hpp"
#include <sqlite3.h>
namespace dbdiff::sqlite::detail {
[[nodiscard]] SchemaSnapshot inspect_database(sqlite3*);
[[nodiscard]] std::string semantic_hash(const SchemaSnapshot&);
[[nodiscard]] std::string render_schema_snapshot(const SchemaSnapshot&);
[[nodiscard]] std::vector<const SchemaObjectSnapshot*> ordered_objects(const SchemaSnapshot&);
} // namespace dbdiff::sqlite::detail
