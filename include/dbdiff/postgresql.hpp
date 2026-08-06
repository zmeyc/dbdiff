#pragma once

#include "dbdiff/backend.hpp"
#include "dbdiff/operation.hpp"
#include "dbdiff/script.hpp"
#include "dbdiff/source.hpp"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dbdiff::postgresql {

[[nodiscard]] BackendKind kind() noexcept;

class ConnectionLocator {
public:
  [[nodiscard]] static ConnectionLocator parse(std::string_view locator);

  [[nodiscard]] ConnectionLocator with_database(std::string_view database) const;
  [[nodiscard]] std::optional<std::string_view> value(std::string_view keyword) const noexcept;
  [[nodiscard]] std::string connection_string() const;
  [[nodiscard]] std::string redacted() const;
  [[nodiscard]] bool empty() const noexcept;

private:
  struct Option {
    std::string keyword;
    std::string value;
    bool secret{false};
  };

  explicit ConnectionLocator(std::vector<Option> options);

  std::vector<Option> options_;
};

struct ServerVersion {
  int number{};
  int major{};
  int patch{};

  auto operator<=>(const ServerVersion&) const = default;
};

[[nodiscard]] ServerVersion validate_server_version(int version_number);
[[nodiscard]] ServerVersion parse_server_version(std::string_view version_number);

[[nodiscard]] std::string quote_identifier(std::string_view identifier);
[[nodiscard]] std::string quote_literal(std::string_view value);

class ScratchName {
public:
  [[nodiscard]] static ScratchName from_token(std::string_view token);
  [[nodiscard]] static ScratchName generate();

  [[nodiscard]] std::string_view value() const noexcept;
  [[nodiscard]] std::string_view token() const noexcept;
  [[nodiscard]] std::string quoted() const;

  auto operator<=>(const ScratchName&) const = default;

private:
  ScratchName(std::string value, std::string token);

  std::string value_;
  std::string token_;
};

class ScratchMarker {
public:
  [[nodiscard]] static ScratchMarker create(std::string_view token,
                                            std::int64_t creation_epoch_seconds);

  [[nodiscard]] std::string_view value() const noexcept;
  [[nodiscard]] std::string quoted() const;

  auto operator<=>(const ScratchMarker&) const = default;

private:
  explicit ScratchMarker(std::string value);

  std::string value_;
};

struct ScratchIdentity {
  ScratchName name;
  ScratchMarker marker;

  [[nodiscard]] static ScratchIdentity generate();
  [[nodiscard]] static ScratchIdentity from_token(std::string_view token,
                                                  std::int64_t creation_epoch_seconds);

  auto operator<=>(const ScratchIdentity&) const = default;
};

struct QualifiedName {
  std::string schema;
  std::string name;

  auto operator<=>(const QualifiedName&) const = default;
};

enum class TablePersistence { permanent, unlogged };
enum class IdentityGeneration { none, always, by_default };
enum class GeneratedStorage { none, stored, virtual_column };

struct Column {
  int position{};
  std::string name;
  std::string type;
  bool not_null{false};
  std::optional<std::string> default_expression;
  IdentityGeneration identity{IdentityGeneration::none};
  GeneratedStorage generated{GeneratedStorage::none};
  std::optional<QualifiedName> collation;

  auto operator<=>(const Column&) const = default;
};

struct Table {
  QualifiedName name;
  TablePersistence persistence{TablePersistence::permanent};
  bool row_security{false};
  bool force_row_security{false};
  std::vector<Column> columns;

  auto operator<=>(const Table&) const = default;
};

struct SchemaSnapshot {
  ServerVersion server_version;
  std::vector<std::string> schemas;
  std::vector<Table> tables;
  std::string semantic_hash;

  auto operator<=>(const SchemaSnapshot&) const = default;
};

[[nodiscard]] SchemaSnapshot normalize_snapshot(SchemaSnapshot snapshot);
[[nodiscard]] std::string semantic_hash(const SchemaSnapshot& snapshot);
[[nodiscard]] SchemaSnapshot introspect_database(const ConnectionLocator& locator,
                                                 const std::vector<std::string>& managed_schemas);

[[nodiscard]] std::vector<StatementSpan> scan_statements(std::string_view sql);
[[nodiscard]] ParsedScript parse_migration(std::string sql);

struct MigrationPlan {
  std::vector<Operation> operations;
  std::vector<std::string> diagnostics;
  bool draft{false};
};

[[nodiscard]] MigrationPlan plan(SchemaSnapshot from, SchemaSnapshot to);
[[nodiscard]] std::string render_plan(const MigrationPlan& migration_plan);

class Database final {
public:
  [[nodiscard]] static Database open(const ConnectionLocator& locator);
  [[nodiscard]] static Database open(std::string_view locator);

  Database(Database&&) noexcept;
  Database& operator=(Database&&) noexcept;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ~Database();

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const ServerVersion& server_version() const;
  [[nodiscard]] SchemaSnapshot introspect(const std::vector<std::string>& managed_schemas) const;

  void execute_source(std::string_view sql);
  void execute_sources(const std::vector<std::string_view>& sources);
  void execute_sources(const SourceSet& sources);
  void execute_migration(std::string_view sql);

private:
  struct Impl;

  explicit Database(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;
};

class ScratchDatabase {
public:
  [[nodiscard]] static ScratchDatabase create(const ConnectionLocator& provisioning_locator);
  [[nodiscard]] static ScratchDatabase create(std::string_view provisioning_locator);

  ScratchDatabase(ScratchDatabase&&) noexcept;
  ScratchDatabase& operator=(ScratchDatabase&&) noexcept;
  ScratchDatabase(const ScratchDatabase&) = delete;
  ScratchDatabase& operator=(const ScratchDatabase&) = delete;
  ~ScratchDatabase();

  [[nodiscard]] const ScratchIdentity& identity() const;
  [[nodiscard]] const ServerVersion& server_version() const;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] SchemaSnapshot introspect(const std::vector<std::string>& managed_schemas) const;

  void execute_source(std::string_view sql);
  void execute_sources(const std::vector<std::string_view>& sources);
  void execute_sources(const SourceSet& sources);
  void execute_migration(std::string_view sql);

  void close();

private:
  struct Impl;

  explicit ScratchDatabase(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> implementation_;
};

} // namespace dbdiff::postgresql
