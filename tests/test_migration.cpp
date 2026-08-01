#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"
#include "dbdiff/migration.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string valid_metadata(const std::string& version = "20260731120000_create_users",
                           const dbdiff::BackendKind backend = dbdiff::BackendKind::sqlite) {
  const auto digest = dbdiff::sha256_hex("state");
  return dbdiff::render_migration_metadata(dbdiff::MigrationMetadata{
      1, backend, version, "3.45.0", digest, digest, digest, {dbdiff::Hazard::write_lock}, false});
}

} // namespace

TEST_CASE("migration metadata has a strict deterministic representation") {
  const auto version = std::string{"20260731120000_create_users"};
  const auto text = valid_metadata(version) + "BEGIN;\nCREATE TABLE users(id INTEGER);\nCOMMIT;\n";
  const auto metadata = dbdiff::parse_migration_metadata(text);

  CHECK(metadata.format == 1);
  CHECK(metadata.backend == dbdiff::BackendKind::sqlite);
  CHECK(metadata.version == version);
  CHECK(metadata.allowed_hazards == dbdiff::HazardSet{dbdiff::Hazard::write_lock});
  CHECK_FALSE(metadata.draft);
  CHECK(dbdiff::render_migration_metadata(metadata) == valid_metadata(version));
}

TEST_CASE("migration metadata rejects malformed and incomplete headers") {
  const auto digest = dbdiff::sha256_hex("state");
  CHECK_THROWS_AS(dbdiff::parse_migration_metadata("CREATE TABLE t(id INTEGER);"), dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::parse_migration_metadata("-- dbdiff: format=1\n-- dbdiff: format=1\n"),
                  dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::parse_migration_metadata("-- dbdiff: format=1\n-- dbdiff: mystery=yes\n"),
                  dbdiff::Error);
  auto header = valid_metadata();
  const auto hash_position = header.find(digest);
  REQUIRE(hash_position != std::string::npos);
  header.replace(hash_position, digest.size(), "ABC");
  CHECK_THROWS_AS(dbdiff::parse_migration_metadata(header), dbdiff::Error);
}

TEST_CASE("migration filenames are constrained and return their version") {
  CHECK(dbdiff::validate_migration_filename("20260731120000_create_users.sql") ==
        "20260731120000_create_users");
  CHECK_THROWS_AS(dbdiff::validate_migration_filename("create_users.sql"), dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::validate_migration_filename("20260731120000_Create.sql"), dbdiff::Error);
  CHECK_THROWS_AS(dbdiff::validate_migration_filename("nested/20260731120000_create_users.sql"),
                  dbdiff::Error);
}

TEST_CASE("migrations load in filename order with exact checksums") {
  dbdiff::test::TempDirectory directory;
  const auto second = std::string{"20260731120001_second"};
  const auto first = std::string{"20260731120000_first"};
  directory.write(second + ".sql", valid_metadata(second) + "BEGIN;\nCOMMIT;\n");
  const auto first_sql = valid_metadata(first) + "BEGIN;\nCOMMIT;\n";
  directory.write(first + ".sql", first_sql);

  const auto migrations = dbdiff::load_migrations(directory.path(), dbdiff::BackendKind::sqlite);
  REQUIRE(migrations.size() == 2);
  CHECK(migrations[0].metadata.version == first);
  CHECK(migrations[1].metadata.version == second);
  CHECK(migrations[0].exact_sha256 == dbdiff::sha256_hex(first_sql));
  CHECK_THROWS_AS(
      dbdiff::load_migration(directory.path() / (first + ".sql"), dbdiff::BackendKind::postgresql),
      dbdiff::Error);
}

TEST_CASE("migration save is atomic and never overwrites") {
  dbdiff::test::TempDirectory directory;
  const auto version = std::string{"20260731120000_create_users"};
  const auto sql = valid_metadata(version) + "BEGIN;\nCOMMIT;\n";

  dbdiff::save_migration_atomic(directory.path() / "migrations", version, sql);
  std::ifstream input{directory.path() / "migrations" / (version + ".sql"), std::ios::binary};
  const std::string saved{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  CHECK(saved == sql);
  CHECK_THROWS_AS(dbdiff::save_migration_atomic(directory.path() / "migrations", version, "new"),
                  dbdiff::Error);
}

TEST_CASE("missing migration directory is an empty stream") {
  dbdiff::test::TempDirectory directory;
  CHECK(dbdiff::load_migrations(directory.path() / "missing", dbdiff::BackendKind::sqlite).empty());
}
