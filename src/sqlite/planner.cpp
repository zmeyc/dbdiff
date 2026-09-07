#include "dbdiff/hash.hpp"
#include "errors.hpp"
#include "schema.hpp"
#include "sql.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dbdiff::sqlite::detail {
[[nodiscard]] bool same_optional_sql(const std::optional<std::string>& left,
                                     const std::optional<std::string>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left.has_value() || canonicalize_schema_sql(*left) == canonicalize_schema_sql(*right);
}

[[nodiscard]] bool same_column(const ColumnSnapshot& left, const ColumnSnapshot& right) {
  return left.rank == right.rank && left.name == right.name &&
         canonicalize_sql(left.declared_type) == canonicalize_sql(right.declared_type) &&
         left.not_null == right.not_null &&
         same_optional_sql(left.default_sql, right.default_sql) &&
         left.primary_key_ordinal == right.primary_key_ordinal && left.generated == right.generated;
}

[[nodiscard]] bool same_table(const TableSnapshot& left, const TableSnapshot& right) {
  if (left.name != right.name || left.without_rowid != right.without_rowid ||
      left.strict != right.strict || left.foreign_keys != right.foreign_keys ||
      canonicalize_schema_sql(left.create_sql) != canonicalize_schema_sql(right.create_sql) ||
      left.columns.size() != right.columns.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.columns.size(); ++index) {
    if (!same_column(left.columns[index], right.columns[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool same_index(const IndexSnapshot& left, const IndexSnapshot& right) {
  return left.name == right.name && left.table == right.table && left.unique == right.unique &&
         left.origin == right.origin && left.partial == right.partial &&
         same_optional_sql(left.create_sql, right.create_sql) && left.columns == right.columns;
}

[[nodiscard]] bool same_object(const SchemaObjectSnapshot& left,
                               const SchemaObjectSnapshot& right) {
  return left.kind == right.kind && left.name == right.name && left.table == right.table &&
         canonicalize_schema_sql(left.create_sql) == canonicalize_schema_sql(right.create_sql);
}

[[nodiscard]] bool safe_added_column(const ColumnSnapshot& column,
                                     const std::string_view definition) {
  if (column.generated != GeneratedColumnKind::ordinary || column.primary_key_ordinal != 0) {
    return false;
  }
  for (const std::string_view keyword : {"primary", "unique", "check", "references", "generated"}) {
    if (contains_unquoted_word(definition, keyword)) {
      return false;
    }
  }
  if (column.default_sql.has_value()) {
    const auto value = trim_sql(*column.default_sql);
    const auto key = ascii_lower(value);
    if (value.starts_with('(') || key == "current_time" || key == "current_date" ||
        key == "current_timestamp") {
      return false;
    }
  }
  if (column.not_null &&
      (!column.default_sql.has_value() || ascii_lower(trim_sql(*column.default_sql)) == "null")) {
    return false;
  }
  return true;
}

[[nodiscard]] std::optional<std::vector<std::string>>
safe_appended_columns(const TableSnapshot& from, const TableSnapshot& to) {
  if (from.name != to.name || from.without_rowid != to.without_rowid || from.strict != to.strict ||
      from.columns.size() >= to.columns.size() || from.foreign_keys != to.foreign_keys) {
    return std::nullopt;
  }
  const auto from_definition = parse_table_definition(from);
  const auto to_definition = parse_table_definition(to);
  if (from_definition.constraints.size() != to_definition.constraints.size() ||
      canonicalize_sql(from_definition.trailing) != canonicalize_sql(to_definition.trailing)) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < from_definition.constraints.size(); ++index) {
    if (canonicalize_sql(from_definition.constraints[index]) !=
        canonicalize_sql(to_definition.constraints[index])) {
      return std::nullopt;
    }
  }
  for (std::size_t index = 0; index < from.columns.size(); ++index) {
    if (!same_column(from.columns[index], to.columns[index]) ||
        canonicalize_sql(from_definition.columns[index]) !=
            canonicalize_sql(to_definition.columns[index])) {
      return std::nullopt;
    }
  }

  std::vector<std::string> additions;
  for (std::size_t index = from.columns.size(); index < to.columns.size(); ++index) {
    if (!safe_added_column(to.columns[index], to_definition.columns[index])) {
      return std::nullopt;
    }
    additions.push_back(to_definition.columns[index]);
  }
  return additions;
}

using TableMap = std::map<std::string, const TableSnapshot*>;
using IndexMap = std::map<std::string, const IndexSnapshot*>;
using ObjectKey = std::pair<SchemaObjectKind, std::string>;
using ObjectMap = std::map<ObjectKey, const SchemaObjectSnapshot*>;

[[nodiscard]] TableMap table_map(const SchemaSnapshot& schema) {
  TableMap result;
  for (const auto& table : schema.tables) {
    result.emplace(ascii_lower(table.name), &table);
  }
  return result;
}

[[nodiscard]] IndexMap explicit_index_map(const SchemaSnapshot& schema) {
  IndexMap result;
  for (const auto& index : schema.indexes) {
    if (index.origin == "c" && index.create_sql.has_value()) {
      result.emplace(ascii_lower(index.name), &index);
    }
  }
  return result;
}

[[nodiscard]] ObjectMap object_map(const SchemaSnapshot& schema) {
  ObjectMap result;
  for (const auto& object : schema.objects) {
    result.emplace(ObjectKey{object.kind, ascii_lower(object.name)}, &object);
  }
  return result;
}

[[nodiscard]] bool table_has_primary_key_index(const SchemaSnapshot& schema,
                                               const TableSnapshot& table) {
  const auto table_key = ascii_lower(table.name);
  return std::ranges::any_of(schema.indexes, [&table_key](const IndexSnapshot& index) {
    return ascii_lower(index.table) == table_key && index.origin == "pk";
  });
}

[[nodiscard]] std::optional<std::string> integer_primary_key_alias(const SchemaSnapshot& schema,
                                                                   const TableSnapshot& table) {
  if (table.without_rowid || table_has_primary_key_index(schema, table)) {
    return std::nullopt;
  }
  const ColumnSnapshot* candidate = nullptr;
  for (const auto& column : table.columns) {
    if (column.primary_key_ordinal <= 0) {
      continue;
    }
    if (candidate != nullptr || column.primary_key_ordinal != 1 ||
        canonicalize_sql(column.declared_type) != canonicalize_sql("INTEGER")) {
      return std::nullopt;
    }
    candidate = &column;
  }
  if (candidate == nullptr) {
    return std::nullopt;
  }
  return candidate->name;
}

[[nodiscard]] std::optional<std::string> accessible_rowid_name(const TableSnapshot& table) {
  std::set<std::string> column_names;
  for (const auto& column : table.columns) {
    column_names.insert(ascii_lower(column.name));
  }
  for (const std::string_view candidate : {"rowid", "_rowid_", "oid"}) {
    if (!column_names.contains(std::string{candidate})) {
      return std::string{candidate};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string temporary_table_name(const SchemaSnapshot& from, const SchemaSnapshot& to,
                                               const std::string_view table) {
  std::set<std::string> existing;
  for (const auto& candidate : from.tables) {
    existing.insert(ascii_lower(candidate.name));
  }
  for (const auto& candidate : to.tables) {
    existing.insert(ascii_lower(candidate.name));
  }

  const auto base = "__dbdiff_new_" + sha256_hex(ascii_lower(table)).substr(0, 12U);
  auto candidate = base;
  std::size_t suffix = 1U;
  while (existing.contains(ascii_lower(candidate))) {
    candidate = base + "_" + std::to_string(suffix++);
  }
  return candidate;
}

[[nodiscard]] std::string create_table_as(const TableSnapshot& table, const std::string_view name) {
  const auto definition = parse_table_definition(table);
  std::string result{"CREATE TABLE "};
  result.append(quote_identifier(name));
  result.append(table.create_sql.substr(definition.open_parenthesis));
  return terminated_statement(result);
}

struct CopyTerm {
  std::string destination;
  std::string source_expression;
};

[[nodiscard]] bool
destination_required_without_value(const ColumnSnapshot& destination,
                                   const std::optional<std::string>& target_ipk) {
  if (!destination.not_null || destination.default_sql.has_value()) {
    return false;
  }
  return !target_ipk.has_value() || ascii_lower(*target_ipk) != ascii_lower(destination.name);
}

void append_sequence_preservation(std::vector<std::string>& statements,
                                  const std::string_view source_table,
                                  const std::string_view temporary_table) {
  const auto source = quote_literal(source_table);
  const auto temporary = quote_literal(temporary_table);

  statements.push_back("UPDATE sqlite_sequence SET seq=max(seq,(SELECT seq FROM "
                       "sqlite_sequence WHERE name=" +
                       source + ")) WHERE name=" + temporary +
                       " AND EXISTS(SELECT 1 FROM sqlite_sequence WHERE name=" + source + ");");
  statements.push_back("INSERT INTO sqlite_sequence(name,seq) SELECT " + temporary +
                       ",seq FROM sqlite_sequence WHERE name=" + source +
                       " AND NOT EXISTS(SELECT 1 FROM sqlite_sequence WHERE name=" + temporary +
                       ");");
}

void append_rebuild(std::vector<std::string>& statements, Plan& result,
                    const SchemaSnapshot& from_schema, const SchemaSnapshot& to_schema,
                    const TableSnapshot& from, const TableSnapshot& to) {
  result.hazards.insert(Hazard::table_rewrite);
  result.hazards.insert(Hazard::constraint_scan);
  const auto temporary = temporary_table_name(from_schema, to_schema, to.name);
  statements.push_back(create_table_as(to, temporary));

  std::map<std::string, const ColumnSnapshot*> source_columns;
  for (const auto& column : from.columns) {
    if (column.generated == GeneratedColumnKind::ordinary) {
      source_columns.emplace(ascii_lower(column.name), &column);
    }
  }
  std::set<std::string> destination_columns;
  std::vector<CopyTerm> copy;
  const auto target_ipk = integer_primary_key_alias(to_schema, to);
  for (const auto& column : to.columns) {
    if (column.generated != GeneratedColumnKind::ordinary) {
      continue;
    }
    const auto key = ascii_lower(column.name);
    destination_columns.insert(key);
    const auto source = source_columns.find(key);
    if (source != source_columns.end()) {
      copy.push_back(CopyTerm{column.name, quote_identifier(source->second->name)});
    } else if (destination_required_without_value(column, target_ipk)) {
      result.draft = true;
    }
  }
  for (const auto& [key, column] : source_columns) {
    static_cast<void>(column);
    if (!destination_columns.contains(key)) {
      result.hazards.insert(Hazard::data_loss);
    }
  }

  if (from.without_rowid != to.without_rowid) {
    result.hazards.insert(Hazard::rowid_reassignment);
  } else if (!from.without_rowid) {
    const auto source_ipk = integer_primary_key_alias(from_schema, from);
    const auto same_ipk = source_ipk.has_value() && target_ipk.has_value() &&
                          ascii_lower(*source_ipk) == ascii_lower(*target_ipk);
    if (!same_ipk && !target_ipk.has_value()) {
      const auto source_rowid = source_ipk.has_value() ? source_ipk : accessible_rowid_name(from);
      const auto destination_rowid = accessible_rowid_name(to);
      if (source_rowid.has_value() && destination_rowid.has_value()) {
        copy.insert(copy.begin(), CopyTerm{*destination_rowid, quote_identifier(*source_rowid)});
      } else {
        result.hazards.insert(Hazard::rowid_reassignment);
      }
    } else if (!same_ipk) {
      result.hazards.insert(Hazard::rowid_reassignment);
    }
  }

  if (copy.empty()) {
    result.draft = true;
  } else {
    std::string insert{"INSERT OR ABORT INTO "};
    insert.append(quote_identifier(temporary));
    insert.push_back('(');
    for (std::size_t index = 0; index < copy.size(); ++index) {
      if (index != 0U) {
        insert.push_back(',');
      }
      insert.append(quote_identifier(copy[index].destination));
    }
    insert.append(") SELECT ");
    for (std::size_t index = 0; index < copy.size(); ++index) {
      if (index != 0U) {
        insert.push_back(',');
      }
      insert.append(copy[index].source_expression);
    }
    insert.append(" FROM ");
    insert.append(quote_identifier(from.name));
    insert.push_back(';');
    statements.push_back(std::move(insert));
  }

  const auto target_autoincrement = contains_unquoted_word(to.create_sql, "autoincrement");
  if (target_autoincrement) {
    append_sequence_preservation(statements, from.name, temporary);
  }
  statements.push_back("DROP TABLE " + quote_identifier(from.name) + ";");
  statements.push_back("ALTER TABLE " + quote_identifier(temporary) + " RENAME TO " +
                       quote_identifier(to.name) + ";");
  if (target_autoincrement) {
    auto final_update = "UPDATE sqlite_sequence SET name=" + quote_literal(to.name) +
                        " WHERE name=" + quote_literal(temporary) + ";";
    statements.push_back(std::move(final_update));
  }
}

enum class TableChangeKind : unsigned char { append_columns, rebuild };

struct TableChange {
  const TableSnapshot* from{nullptr};
  const TableSnapshot* to{nullptr};
  TableChangeKind kind{TableChangeKind::rebuild};
  std::vector<std::string> additions;
};

[[nodiscard]] bool key_in(const std::set<std::string>& keys, const std::string_view name) {
  return keys.contains(ascii_lower(name));
}

void append_drop_objects(std::vector<std::string>& statements,
                         const std::vector<const SchemaObjectSnapshot*>& objects,
                         const SchemaObjectKind kind) {
  for (const auto* object : objects) {
    if (object->kind != kind) {
      continue;
    }
    const auto type = kind == SchemaObjectKind::view ? "VIEW " : "TRIGGER ";
    statements.push_back("DROP " + std::string{type} + quote_identifier(object->name) + ";");
  }
}

void append_create_objects(std::vector<std::string>& statements,
                           const std::vector<const SchemaObjectSnapshot*>& objects,
                           const SchemaObjectKind kind) {
  for (const auto* object : objects) {
    if (object->kind == kind) {
      statements.push_back(terminated_statement(object->create_sql));
    }
  }
}

[[nodiscard]] Plan make_plan(const SchemaSnapshot& from, const SchemaSnapshot& to) {
  Plan result;
  const auto from_tables = table_map(from);
  const auto to_tables = table_map(to);
  std::vector<const TableSnapshot*> removed_tables;
  std::vector<const TableSnapshot*> added_tables;
  std::vector<TableChange> changed_tables;
  std::set<std::string> destructive_tables;
  std::set<std::string> rebuilt_tables;
  std::set<std::string> added_table_keys;

  for (const auto& [key, table] : from_tables) {
    const auto desired = to_tables.find(key);
    if (desired == to_tables.end()) {
      removed_tables.push_back(table);
      destructive_tables.insert(key);
      result.hazards.insert(Hazard::data_loss);
      continue;
    }
    if (same_table(*table, *desired->second)) {
      continue;
    }
    if (auto additions = safe_appended_columns(*table, *desired->second); additions.has_value()) {
      changed_tables.push_back(
          TableChange{table, desired->second, TableChangeKind::append_columns, *additions});
    } else {
      changed_tables.push_back(TableChange{table, desired->second, TableChangeKind::rebuild, {}});
      destructive_tables.insert(key);
      rebuilt_tables.insert(key);
    }
  }
  for (const auto& [key, table] : to_tables) {
    if (!from_tables.contains(key)) {
      added_tables.push_back(table);
      added_table_keys.insert(key);
    }
  }

  const auto from_indexes = explicit_index_map(from);
  const auto to_indexes = explicit_index_map(to);
  const auto from_objects = object_map(from);
  const auto to_objects = object_map(to);
  const auto rebuilds_schema_references = !destructive_tables.empty();
  std::vector<std::string> statements;

  if (rebuilds_schema_references) {
    const auto current_objects = ordered_objects(from);
    append_drop_objects(statements, current_objects, SchemaObjectKind::trigger);
    append_drop_objects(statements, current_objects, SchemaObjectKind::view);
  } else {
    std::vector<const SchemaObjectSnapshot*> drops;
    for (const auto& [key, object] : from_objects) {
      const auto desired = to_objects.find(key);
      if (desired == to_objects.end() || !same_object(*object, *desired->second)) {
        drops.push_back(object);
      }
    }
    append_drop_objects(statements, drops, SchemaObjectKind::trigger);
    append_drop_objects(statements, drops, SchemaObjectKind::view);
  }

  for (const auto& [key, index] : from_indexes) {
    if (key_in(destructive_tables, index->table)) {
      continue;
    }
    const auto desired = to_indexes.find(key);
    if (desired == to_indexes.end() || !same_index(*index, *desired->second)) {
      statements.push_back("DROP INDEX " + quote_identifier(index->name) + ";");
    }
  }

  for (const auto* table : removed_tables) {
    statements.push_back("DROP TABLE " + quote_identifier(table->name) + ";");
  }
  for (const auto& change : changed_tables) {
    if (change.kind == TableChangeKind::append_columns) {
      for (const auto& addition : change.additions) {
        statements.push_back("ALTER TABLE " + quote_identifier(change.from->name) + " ADD COLUMN " +
                             addition + ";");
      }
    } else {
      append_rebuild(statements, result, from, to, *change.from, *change.to);
    }
  }
  for (const auto* table : added_tables) {
    statements.push_back(terminated_statement(table->create_sql));
  }

  for (const auto& [key, index] : to_indexes) {
    const auto current = from_indexes.find(key);
    const auto table_recreated =
        key_in(rebuilt_tables, index->table) || key_in(added_table_keys, index->table);
    if (table_recreated || current == from_indexes.end() || !same_index(*current->second, *index)) {
      statements.push_back(terminated_statement(index->create_sql.value_or(std::string{})));
    }
  }

  if (rebuilds_schema_references) {
    const auto desired_objects = ordered_objects(to);
    append_create_objects(statements, desired_objects, SchemaObjectKind::view);
    append_create_objects(statements, desired_objects, SchemaObjectKind::trigger);
  } else {
    std::vector<const SchemaObjectSnapshot*> creates;
    for (const auto& [key, object] : to_objects) {
      const auto current = from_objects.find(key);
      if (current == from_objects.end() || !same_object(*current->second, *object)) {
        creates.push_back(object);
      }
    }
    append_create_objects(statements, creates, SchemaObjectKind::view);
    append_create_objects(statements, creates, SchemaObjectKind::trigger);
  }

  if (statements.empty()) {
    return result;
  }
  result.hazards.insert(Hazard::write_lock);
  std::string sql;
  if (result.draft) {
    sql.append("-- dbdiff:draft\n");
  }
  if (rebuilds_schema_references) {
    sql.append("PRAGMA foreign_keys=OFF;\n");
  }
  sql.append("BEGIN IMMEDIATE;\n");
  for (const auto& statement : statements) {
    sql.append(statement);
    sql.push_back('\n');
  }
  if (rebuilds_schema_references) {
    sql.append("PRAGMA foreign_key_check;\n");
  }
  sql.append("COMMIT;\n");
  if (rebuilds_schema_references) {
    sql.append("PRAGMA foreign_keys=ON;\n");
  }
  result.sql = std::move(sql);
  return result;
}

} // namespace dbdiff::sqlite::detail

namespace dbdiff::sqlite {
using namespace detail;
Plan plan(const SchemaSnapshot& from, const SchemaSnapshot& to) { return make_plan(from, to); }

bool validate_plan(const SchemaSnapshot& from, const SchemaSnapshot& to) {
  auto database = Database::temporary();
  const auto baseline = render_schema_snapshot(from);
  if (!baseline.empty()) {
    database.execute_migration(baseline);
  }
  if (database.inspect().semantic_hash != semantic_hash(from)) {
    return false;
  }

  const auto candidate = make_plan(from, to);
  if (!candidate.sql.empty()) {
    database.execute_migration(candidate.sql);
  }
  return database.inspect().semantic_hash == semantic_hash(to);
}

} // namespace dbdiff::sqlite
