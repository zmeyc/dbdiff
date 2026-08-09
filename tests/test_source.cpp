#include "dbdiff/error.hpp"
#include "dbdiff/source.hpp"

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace {

dbdiff::SourceResolver resolver(const dbdiff::test::TempDirectory& temporary) {
  return {temporary.path(), temporary.path() / "migrations", [] { return "SELECT 1;\n"; }};
}

} // namespace

TEST_CASE("directory sources are recursively resolved in lexical order", "[unit][SRC-001]") {
  dbdiff::test::TempDirectory temporary;
  temporary.write("schema/z.sql", "CREATE TABLE z(id integer);\n");
  temporary.write("schema/a/2.sql", "CREATE TABLE a2(id integer);\n");
  temporary.write("schema/a/1.sql", "CREATE TABLE a1(id integer);\n");
  temporary.write("schema/ignored.txt", "ignored");

  const auto sources = resolver(temporary).resolve({"schema"});
  REQUIRE(sources.files.size() == 3);
  CHECK(sources.files[0].display_path == "schema/a/1.sql");
  CHECK(sources.files[1].display_path == "schema/a/2.sql");
  CHECK(sources.files[2].display_path == "schema/z.sql");
  CHECK(sources.exact_sha256.size() == 64);
}

TEST_CASE("root and nested manifests define exact source order", "[unit][SRC-002]") {
  dbdiff::test::TempDirectory temporary;
  temporary.write("schema/dbdiff.schema", "utils.sql\nauth/auth.dbdiff-schema\n");
  temporary.write("schema/utils.sql", "CREATE TABLE utils(id integer);\n");
  temporary.write("schema/auth/auth.dbdiff-schema", "roles.sql\nusers.sql # inline comment\n");
  temporary.write("schema/auth/users.sql", "CREATE TABLE users(id integer);\n");
  temporary.write("schema/auth/roles.sql", "CREATE TABLE roles(id integer);\n");

  const auto sources = resolver(temporary).resolve({"schema"});
  REQUIRE(sources.files.size() == 3);
  CHECK(sources.files[0].display_path == "schema/utils.sql");
  CHECK(sources.files[1].display_path == "schema/auth/roles.sql");
  CHECK(sources.files[2].display_path == "schema/auth/users.sql");
}

TEST_CASE("source resolver rejects unsafe and ambiguous inputs", "[unit][SRC-003]") {
  dbdiff::test::TempDirectory temporary;
  temporary.write("schema/a.sql", "SELECT 1;\n");

  SECTION("duplicate") {
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/a.sql", "schema/a.sql"}), dbdiff::Error);
  }
  SECTION("manifest cycle") {
    temporary.write("schema/one.dbdiff-schema", "two.dbdiff-schema\n");
    temporary.write("schema/two.dbdiff-schema", "one.dbdiff-schema\n");
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/one.dbdiff-schema"}), dbdiff::Error);
  }
  SECTION("parent traversal") {
    temporary.write("schema/bad.dbdiff-schema", "../outside.sql\n");
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/bad.dbdiff-schema"}), dbdiff::Error);
  }
  SECTION("migration overlap") {
    temporary.write("migrations/schema.sql", "SELECT 1;\n");
    CHECK_THROWS_AS(resolver(temporary).resolve({"migrations/schema.sql"}), dbdiff::Error);
  }
  SECTION("stdin twice") {
    CHECK_THROWS_AS(resolver(temporary).resolve({"-", "-"}), dbdiff::Error);
  }
  SECTION("symbolic link") {
    std::error_code error;
    std::filesystem::create_symlink(temporary.path() / "schema/a.sql",
                                    temporary.path() / "schema/link.sql", error);
    REQUIRE_FALSE(error);
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/link.sql"}), dbdiff::Error);
  }
  SECTION("implicit manifest symbolic link") {
    temporary.write("schema/manifest.sql", "a.sql\n");
    std::error_code error;
    std::filesystem::create_symlink(temporary.path() / "schema/manifest.sql",
                                    temporary.path() / "schema/dbdiff.schema", error);
    REQUIRE_FALSE(error);
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema"}), dbdiff::Error);
  }
  SECTION("symbolic parent outside the project") {
    temporary.write("outside/a.sql", "SELECT 1;\n");
    std::error_code error;
    std::filesystem::create_directory_symlink(temporary.path() / "outside",
                                              temporary.path() / "linked-parent", error);
    REQUIRE_FALSE(error);
    CHECK_THROWS_AS(resolver(temporary).resolve({temporary.path() / "linked-parent/a.sql"}),
                    dbdiff::Error);
  }
  SECTION("invalid UTF-8") {
    temporary.write("schema/invalid.sql", std::string{"\xc0\xaf", 2});
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/invalid.sql"}), dbdiff::Error);
  }
  SECTION("invalid UTF-8 manifest") {
    temporary.write("schema/invalid.dbdiff-schema", std::string{"\xc0\xaf", 2});
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/invalid.dbdiff-schema"}), dbdiff::Error);
  }
  SECTION("glob in manifest") {
    temporary.write("schema/glob.dbdiff-schema", "*.sql\n");
    CHECK_THROWS_AS(resolver(temporary).resolve({"schema/glob.dbdiff-schema"}), dbdiff::Error);
  }
}

TEST_CASE("source fingerprints include order, path, and exact bytes", "[unit][SRC-004]") {
  dbdiff::test::TempDirectory temporary;
  temporary.write("a.sql", "SELECT 1;\n");
  temporary.write("b.sql", "SELECT 2;\n");
  const auto source_resolver = resolver(temporary);

  const auto first = source_resolver.resolve({"a.sql", "b.sql"});
  const auto repeated = source_resolver.resolve({"a.sql", "b.sql"});
  const auto reordered = source_resolver.resolve({"b.sql", "a.sql"});
  CHECK(first.exact_sha256 == repeated.exact_sha256);
  CHECK(first.exact_sha256 != reordered.exact_sha256);

  temporary.write("b.sql", "SELECT 2;  \n");
  CHECK(first.exact_sha256 != source_resolver.resolve({"a.sql", "b.sql"}).exact_sha256);
}

TEST_CASE("UTF-8 validation rejects malformed and overlong encodings", "[unit][SRC-003]") {
  CHECK(dbdiff::is_valid_utf8("plain ASCII"));
  CHECK(dbdiff::is_valid_utf8("საქართველო"));
  CHECK_FALSE(dbdiff::is_valid_utf8(std::string{"\xc0\xaf", 2}));
  CHECK_FALSE(dbdiff::is_valid_utf8(std::string{"\xed\xa0\x80", 3}));
  CHECK_FALSE(dbdiff::is_valid_utf8(std::string{"\xf4\x90\x80\x80", 4}));
}
