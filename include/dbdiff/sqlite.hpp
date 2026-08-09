#pragma once

#include "dbdiff/backend.hpp"
#include "dbdiff/hazard.hpp"
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

struct Plan {
  std::string sql;
  HazardSet hazards;
  bool draft{false};

  bool operator==(const Plan&) const = default;
};

enum class MigrationUnitState { started, completed };

struct MigrationUnitRecord {
  std::size_t ordinal{0};
  std::string exact_sha256;
  bool explicit_transaction{false};
  std::string before_schema_sha256;
  std::string after_schema_sha256;
  MigrationUnitState state{MigrationUnitState::started};

  bool operator==(const MigrationUnitRecord&) const = default;
};

struct MigrationHistoryEntry {
  std::string version;
  std::string backend;
  std::string engine_version;
  std::string attempted_file_sha256;
  std::optional<std::string> completed_file_sha256;
  std::vector<MigrationUnitRecord> units;

  bool operator==(const MigrationHistoryEntry&) const = default;
};

struct MigrationHistory {
  bool initialized{false};
  std::vector<MigrationHistoryEntry> entries;

  bool operator==(const MigrationHistory&) const = default;
};

struct MigrationRevision {
  std::size_t ordinal{0};
  std::string exact_file_sha256;
  std::string sql;

  bool operator==(const MigrationRevision&) const = default;
};

struct MigrationApplyResult {
  bool already_completed{false};
  std::size_t completed_unit_count{0};
  std::string completed_file_sha256;

  bool operator==(const MigrationApplyResult&) const = default;
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
  void execute_prefix(std::string_view sql, std::size_t completed_unit_count);
  void backup_to(Database& destination) const;
  [[nodiscard]] MigrationHistory read_history() const;
  [[nodiscard]] MigrationApplyResult apply_version(std::string_view version,
                                                   std::string_view exact_file_sha256,
                                                   std::string_view sql, bool resume);
  [[nodiscard]] std::vector<MigrationRevision> recover_revisions(std::string_view version) const;
  [[nodiscard]] SchemaSnapshot inspect() const;

private:
  struct Impl;

  explicit Database(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] std::vector<StatementSpan> scan_statements(std::string_view sql);
[[nodiscard]] Plan plan(const SchemaSnapshot& from, const SchemaSnapshot& to);
[[nodiscard]] std::string render_snapshot(const SchemaSnapshot& snapshot);
[[nodiscard]] bool validate_plan(const SchemaSnapshot& from, const SchemaSnapshot& to);
[[nodiscard]] BackendKind kind() noexcept;

} // namespace dbdiff::sqlite
