#include "schema.hpp"
#include "connection.hpp"
#include "database_impl.hpp"
#include "dbdiff/hash.hpp"
#include "errors.hpp"
#include "sql.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dbdiff::sqlite::detail {
[[nodiscard]] std::string optional_key(const std::optional<std::string>& value) {
  return value.has_value() ? *value : std::string{};
}

template <typename Item, typename Projection>
void stable_sort(std::vector<Item>& values, Projection projection) {
  std::ranges::sort(values, [&projection](const Item& left, const Item& right) {
    const auto left_value = projection(left);
    const auto right_value = projection(right);
    const auto left_key = ascii_lower(left_value);
    const auto right_key = ascii_lower(right_value);
    return std::tie(left_key, left_value) < std::tie(right_key, right_value);
  });
}

[[nodiscard]] std::map<std::pair<std::string, std::string>, std::optional<std::string>>
schema_sql(sqlite3* database) {
  std::map<std::pair<std::string, std::string>, std::optional<std::string>> result;
  Statement statement{database, "SELECT type,name,sql FROM main.sqlite_schema "
                                "WHERE type IN ('table','index','view','trigger') "
                                "ORDER BY type COLLATE BINARY,name COLLATE BINARY;"};
  while (statement.step() == SQLITE_ROW) {
    auto type = statement.text(0);
    auto name = statement.text(1);
    result.emplace(std::pair{std::move(type), std::move(name)}, statement.optional_text(2));
  }
  return result;
}

[[nodiscard]] std::vector<ColumnSnapshot> inspect_columns(sqlite3* database,
                                                          const std::string_view table) {
  Statement statement{database, "SELECT cid,name,type,\"notnull\",dflt_value,pk,hidden "
                                "FROM pragma_table_xinfo(?1,'main') ORDER BY cid;"};
  statement.bind_text(1, table);
  std::vector<ColumnSnapshot> columns;
  while (statement.step() == SQLITE_ROW) {
    const auto hidden = statement.integer(6);
    GeneratedColumnKind generated{};
    switch (hidden) {
    case 0:
      generated = GeneratedColumnKind::ordinary;
      break;
    case 2:
      generated = GeneratedColumnKind::virtual_generated;
      break;
    case 3:
      generated = GeneratedColumnKind::stored_generated;
      break;
    default:
      throw_unsupported("unsupported hidden SQLite column in table " + std::string{table});
    }
    columns.push_back(ColumnSnapshot{statement.integer(0), statement.text(1), statement.text(2),
                                     statement.integer(3) != 0, statement.optional_text(4),
                                     statement.integer(5), generated});
  }
  return columns;
}

[[nodiscard]] std::vector<ForeignKeySnapshot> inspect_foreign_keys(sqlite3* database,
                                                                   const std::string_view table) {
  Statement statement{database, "SELECT id,seq,\"table\",\"from\",\"to\",on_update,on_delete,match "
                                "FROM pragma_foreign_key_list(?1,'main') ORDER BY id,seq;"};
  statement.bind_text(1, table);

  std::vector<ForeignKeySnapshot> foreign_keys;
  while (statement.step() == SQLITE_ROW) {
    const auto id = statement.integer(0);
    if (foreign_keys.empty() || foreign_keys.back().id != id) {
      foreign_keys.push_back(ForeignKeySnapshot{
          id, statement.text(2), statement.text(5), statement.text(6), statement.text(7), {}});
    }
    foreign_keys.back().columns.push_back(ForeignKeyColumnSnapshot{
        statement.integer(1), statement.text(3), statement.optional_text(4)});
  }
  return foreign_keys;
}

[[nodiscard]] std::vector<IndexColumnSnapshot> inspect_index_columns(sqlite3* database,
                                                                     const std::string_view index) {
  Statement statement{database, "SELECT seqno,cid,name,\"desc\",coll,\"key\" "
                                "FROM pragma_index_xinfo(?1,'main') ORDER BY seqno;"};
  statement.bind_text(1, index);
  std::vector<IndexColumnSnapshot> columns;
  while (statement.step() == SQLITE_ROW) {
    columns.push_back(IndexColumnSnapshot{statement.integer(0), statement.integer(1),
                                          statement.optional_text(2), statement.integer(3) != 0,
                                          statement.optional_text(4), statement.integer(5) != 0});
  }
  return columns;
}

[[nodiscard]] std::vector<IndexSnapshot> inspect_indexes(
    sqlite3* database, const std::vector<TableSnapshot>& tables,
    const std::map<std::pair<std::string, std::string>, std::optional<std::string>>& definitions) {
  std::vector<IndexSnapshot> indexes;
  for (const auto& table : tables) {
    Statement statement{database, "SELECT name,\"unique\",origin,partial "
                                  "FROM pragma_index_list(?1,'main') ORDER BY seq;"};
    statement.bind_text(1, table.name);
    while (statement.step() == SQLITE_ROW) {
      auto name = statement.text(0);
      std::optional<std::string> create_sql;
      const auto definition = definitions.find({"index", name});
      if (definition != definitions.end()) {
        create_sql = definition->second;
      }
      indexes.push_back(IndexSnapshot{std::move(name),
                                      table.name,
                                      statement.integer(1) != 0,
                                      statement.text(2),
                                      statement.integer(3) != 0,
                                      std::move(create_sql),
                                      {}});
      indexes.back().columns = inspect_index_columns(database, indexes.back().name);
    }
  }
  std::ranges::sort(indexes, [](const IndexSnapshot& left, const IndexSnapshot& right) {
    return std::tuple{ascii_lower(left.table), ascii_lower(left.name), left.table, left.name} <
           std::tuple{ascii_lower(right.table), ascii_lower(right.name), right.table, right.name};
  });
  return indexes;
}

void hash_text(Sha256& hash, const std::string_view name, const std::string_view value) {
  hash.add_length_prefixed(name);
  hash.add_length_prefixed(value);
}

void hash_integer(Sha256& hash, const std::string_view name, const int value) {
  hash_text(hash, name, std::to_string(value));
}

void hash_boolean(Sha256& hash, const std::string_view name, const bool value) {
  hash_text(hash, name, value ? "1" : "0");
}

void hash_optional_sql(Sha256& hash, const std::string_view name,
                       const std::optional<std::string>& value) {
  hash_boolean(hash, "present", value.has_value());
  if (value.has_value()) {
    hash_text(hash, name, canonicalize_schema_sql(*value));
  }
}

[[nodiscard]] std::string semantic_hash(const SchemaSnapshot& schema) {
  Sha256 hash;
  hash.add_length_prefixed("dbdiff.sqlite.schema.v2");
  for (const auto& table : schema.tables) {
    hash_text(hash, "object", "table");
    hash_text(hash, "name", table.name);
    hash_text(hash, "sql", canonicalize_schema_sql(table.create_sql));
    hash_boolean(hash, "without_rowid", table.without_rowid);
    hash_boolean(hash, "strict", table.strict);
    for (const auto& column : table.columns) {
      hash_text(hash, "child", "column");
      hash_integer(hash, "rank", column.rank);
      hash_text(hash, "name", column.name);
      hash_text(hash, "type", canonicalize_sql(column.declared_type));
      hash_boolean(hash, "not_null", column.not_null);
      hash_optional_sql(hash, "default", column.default_sql);
      hash_integer(hash, "pk", column.primary_key_ordinal);
      hash_integer(hash, "generated", static_cast<int>(column.generated));
    }
    for (const auto& foreign_key : table.foreign_keys) {
      hash_text(hash, "child", "foreign_key");
      hash_integer(hash, "id", foreign_key.id);
      hash_text(hash, "parent", foreign_key.parent_table);
      hash_text(hash, "on_update", foreign_key.on_update);
      hash_text(hash, "on_delete", foreign_key.on_delete);
      hash_text(hash, "match", foreign_key.match);
      for (const auto& column : foreign_key.columns) {
        hash_integer(hash, "sequence", column.sequence);
        hash_text(hash, "from", column.from_column);
        hash_boolean(hash, "to_present", column.to_column.has_value());
        hash_text(hash, "to", optional_key(column.to_column));
      }
    }
  }
  for (const auto& index : schema.indexes) {
    hash_text(hash, "object", "index");
    hash_text(hash, "name", index.name);
    hash_text(hash, "table", index.table);
    hash_boolean(hash, "unique", index.unique);
    hash_text(hash, "origin", index.origin);
    hash_boolean(hash, "partial", index.partial);
    hash_optional_sql(hash, "sql", index.create_sql);
    for (const auto& column : index.columns) {
      hash_integer(hash, "sequence", column.sequence);
      hash_integer(hash, "table_column", column.table_column_rank);
      hash_boolean(hash, "name_present", column.name.has_value());
      hash_text(hash, "name", optional_key(column.name));
      hash_boolean(hash, "descending", column.descending);
      hash_boolean(hash, "collation_present", column.collation.has_value());
      hash_text(hash, "collation", optional_key(column.collation));
      hash_boolean(hash, "key", column.key);
    }
  }
  for (const auto& object : schema.objects) {
    hash_text(hash, "object", object.kind == SchemaObjectKind::view ? "view" : "trigger");
    hash_text(hash, "name", object.name);
    hash_text(hash, "table", object.table);
    hash_text(hash, "sql", canonicalize_schema_sql(object.create_sql));
  }
  return hash.finish_hex();
}

[[nodiscard]] SchemaSnapshot inspect_database(sqlite3* database) {
  DeferredForeignKeysGuard preserve_deferral{database};
  {
    Statement attached{database, "PRAGMA database_list;"};
    while (attached.step() == SQLITE_ROW) {
      const auto name = attached.text(1);
      if (name != "main" && name != "temp") {
        throw_unsupported("attached SQLite database is not supported: " + name);
      }
    }
  }
  if (query_integer(database, "SELECT count(*) FROM temp.sqlite_schema;") != 0) {
    throw_unsupported("temporary SQLite schema objects are not supported");
  }

  const auto definitions = schema_sql(database);
  for (const auto& [identity, definition] : definitions) {
    static_cast<void>(definition);
    const auto& [type, name] = identity;
    if (is_dbdiff_reserved_name(name) && !(type == "table" && is_history_table(name))) {
      throw_unsupported("unrecognized reserved SQLite schema object: " + name);
    }
  }
  SchemaSnapshot snapshot;
  Statement tables{database, "SELECT name,type,wr,strict FROM pragma_table_list "
                             "WHERE schema='main' ORDER BY name COLLATE BINARY;"};
  while (tables.step() == SQLITE_ROW) {
    const auto name = tables.text(0);
    const auto type = tables.text(1);
    if (type == "virtual" || type == "shadow") {
      throw_unsupported("virtual and shadow SQLite tables are not supported: " + name);
    }
    if (type != "table" || starts_with_reserved_name(name)) {
      continue;
    }
    const auto definition = definitions.find({"table", name});
    if (definition == definitions.end() || !definition->second.has_value()) {
      throw_unsupported("SQLite table has no inspectable CREATE statement: " + name);
    }
    snapshot.tables.push_back(TableSnapshot{name, definition->second.value_or(std::string{}),
                                            tables.integer(2) != 0, tables.integer(3) != 0,
                                            inspect_columns(database, name),
                                            inspect_foreign_keys(database, name)});
  }
  stable_sort(snapshot.tables, [](const TableSnapshot& table) { return table.name; });
  snapshot.indexes = inspect_indexes(database, snapshot.tables, definitions);

  Statement objects{database, "SELECT type,name,tbl_name,sql FROM main.sqlite_schema "
                              "WHERE type IN ('view','trigger') "
                              "ORDER BY type COLLATE BINARY,name COLLATE BINARY;"};
  while (objects.step() == SQLITE_ROW) {
    const auto type = objects.text(0);
    const auto name = objects.text(1);
    if (starts_with_reserved_name(name)) {
      continue;
    }
    const auto sql = objects.optional_text(3);
    if (!sql.has_value()) {
      throw_unsupported("SQLite schema object has no inspectable CREATE statement: " + name);
    }
    snapshot.objects.push_back(
        SchemaObjectSnapshot{type == "view" ? SchemaObjectKind::view : SchemaObjectKind::trigger,
                             name, objects.text(2), *sql});
  }
  std::ranges::sort(snapshot.objects,
                    [](const SchemaObjectSnapshot& left, const SchemaObjectSnapshot& right) {
                      return std::tuple{left.kind, ascii_lower(left.name), left.name} <
                             std::tuple{right.kind, ascii_lower(right.name), right.name};
                    });
  snapshot.semantic_hash = semantic_hash(snapshot);
  return snapshot;
}

[[nodiscard]] std::vector<const SchemaObjectSnapshot*>
ordered_objects(const SchemaSnapshot& schema) {
  std::vector<const SchemaObjectSnapshot*> result;
  result.reserve(schema.objects.size());
  for (const auto& object : schema.objects) {
    result.push_back(&object);
  }
  std::ranges::sort(result, [](const auto* left, const auto* right) {
    return std::tuple{left->kind, ascii_lower(left->name), left->name} <
           std::tuple{right->kind, ascii_lower(right->name), right->name};
  });
  return result;
}

[[nodiscard]] std::string render_schema_snapshot(const SchemaSnapshot& snapshot) {
  std::vector<std::string> statements;
  statements.reserve(snapshot.tables.size() + snapshot.indexes.size() + snapshot.objects.size());
  for (const auto& table : snapshot.tables) {
    statements.push_back(terminated_statement(table.create_sql));
  }
  for (const auto& index : snapshot.indexes) {
    if (index.origin == "c" && index.create_sql.has_value()) {
      statements.push_back(terminated_statement(*index.create_sql));
    }
  }
  const auto objects = ordered_objects(snapshot);
  for (const auto* object : objects) {
    statements.push_back(terminated_statement(object->create_sql));
  }
  if (statements.empty()) {
    return {};
  }

  std::string result{"BEGIN IMMEDIATE;\n"};
  for (const auto& statement : statements) {
    result.append(statement);
    result.push_back('\n');
  }
  result.append("PRAGMA foreign_key_check;\nCOMMIT;\n");
  return result;
}

} // namespace dbdiff::sqlite::detail

namespace dbdiff::sqlite {
using namespace detail;
SchemaSnapshot Database::inspect() const {
  OperationDeadline deadline{implementation_->progress};
  return inspect_database(implementation_->handle);
}

std::string render_snapshot(const SchemaSnapshot& snapshot) {
  return render_schema_snapshot(snapshot);
}

} // namespace dbdiff::sqlite
