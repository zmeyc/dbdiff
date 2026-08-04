#pragma once

#include "dbdiff/backend.hpp"
#include "dbdiff/script.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff::sqlite {

enum class OpenMode { read_only, read_write, read_write_create };

enum class GeneratedColumnKind {
  ordinary = 0,
  virtual_generated = 2,
  stored_generated = 3,
};

struct ColumnSnapshot {
  int rank{0};
  std::string name;
  std::string declared_type;
  bool not_null{false};
  std::optional<std::string> default_sql;
  int primary_key_ordinal{0};
  GeneratedColumnKind generated{GeneratedColumnKind::ordinary};

  bool operator==(const ColumnSnapshot&) const = default;
};

struct ForeignKeyColumnSnapshot {
  int sequence{0};
  std::string from_column;
  std::optional<std::string> to_column;

  bool operator==(const ForeignKeyColumnSnapshot&) const = default;
};

struct ForeignKeySnapshot {
  int id{0};
  std::string parent_table;
  std::string on_update;
  std::string on_delete;
  std::string match;
  std::vector<ForeignKeyColumnSnapshot> columns;

  bool operator==(const ForeignKeySnapshot&) const = default;
};

struct IndexColumnSnapshot {
  int sequence{0};
  int table_column_rank{0};
  std::optional<std::string> name;
  bool descending{false};
  std::optional<std::string> collation;
  bool key{false};

  bool operator==(const IndexColumnSnapshot&) const = default;
};

struct IndexSnapshot {
  std::string name;
  std::string table;
  bool unique{false};
  std::string origin;
  bool partial{false};
  std::optional<std::string> create_sql;
  std::vector<IndexColumnSnapshot> columns;

  bool operator==(const IndexSnapshot&) const = default;
};

struct TableSnapshot {
  std::string name;
  std::string create_sql;
  bool without_rowid{false};
  bool strict{false};
  std::vector<ColumnSnapshot> columns;
  std::vector<ForeignKeySnapshot> foreign_keys;

  bool operator==(const TableSnapshot&) const = default;
};

enum class SchemaObjectKind { view, trigger };

struct SchemaObjectSnapshot {
  SchemaObjectKind kind{SchemaObjectKind::view};
  std::string name;
  std::string table;
  std::string create_sql;

  bool operator==(const SchemaObjectSnapshot&) const = default;
};

struct SchemaSnapshot {
  std::vector<TableSnapshot> tables;
  std::vector<IndexSnapshot> indexes;
  std::vector<SchemaObjectSnapshot> objects;
  std::string semantic_hash;

  bool operator==(const SchemaSnapshot&) const = default;
};

class Database final {
public:
  [[nodiscard]] static Database temporary();
  [[nodiscard]] static Database open(const std::filesystem::path& path, OpenMode mode);

  ~Database();
  Database(Database&&) noexcept;
  Database& operator=(Database&&) noexcept;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void execute_source(std::string_view sql);
  void execute_migration(std::string_view sql);
  [[nodiscard]] SchemaSnapshot inspect() const;

private:
  struct Impl;

  explicit Database(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] std::vector<StatementSpan> scan_statements(std::string_view sql);
[[nodiscard]] BackendKind kind() noexcept;

} // namespace dbdiff::sqlite
