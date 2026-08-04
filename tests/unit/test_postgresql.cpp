#include "dbdiff/error.hpp"
#include "dbdiff/postgresql.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pg = dbdiff::postgresql;

TEST_CASE("PostgreSQL locator parsing is deterministic and redacts credentials",
          "[unit][postgresql]") {
  const auto locator = pg::ConnectionLocator::parse(
      "postgresql://alice:p%40ss@localhost:5432/old_db?application_name=dbdiff");

  REQUIRE(locator.value("user").has_value());
  CHECK(locator.value("user").value_or("") == "alice");
  REQUIRE(locator.value("password").has_value());
  CHECK(locator.value("password").value_or("") == "p@ss");
  REQUIRE(locator.value("dbname").has_value());
  CHECK(locator.value("dbname").value_or("") == "old_db");

  const auto rewritten = locator.with_database("new database");
  REQUIRE(rewritten.value("dbname").has_value());
  CHECK(rewritten.value("dbname").value_or("") == "new database");
  CHECK(rewritten.connection_string().find("dbname='new database'") != std::string::npos);
  CHECK(rewritten.connection_string().find("password='p@ss'") != std::string::npos);
  CHECK(rewritten.redacted().find("p@ss") == std::string::npos);
  CHECK(rewritten.redacted().find("<redacted>") != std::string::npos);
}

TEST_CASE("PostgreSQL keyword locators preserve quoted option values", "[unit][postgresql]") {
  const auto locator = pg::ConnectionLocator::parse("host='local host' password='a\\\\b\\'c'");

  REQUIRE(locator.value("host").has_value());
  CHECK(locator.value("host").value_or("") == "local host");
  REQUIRE(locator.value("password").has_value());
  CHECK(locator.value("password").value_or("") == "a\\b'c");

  const auto reparsed = pg::ConnectionLocator::parse(locator.connection_string());
  CHECK(reparsed.value("host") == locator.value("host"));
  CHECK(reparsed.value("password") == locator.value("password"));

  const auto environment_locator = pg::ConnectionLocator::parse("");
  CHECK(environment_locator.empty());
  const auto environment_with_database = environment_locator.with_database("scratch");
  CHECK_FALSE(environment_with_database.empty());
  REQUIRE(environment_with_database.value("dbname").has_value());
  CHECK(environment_with_database.value("dbname").value_or("") == "scratch");
}

TEST_CASE("PostgreSQL locator errors never repeat the supplied secret", "[unit][postgresql]") {
  try {
    static_cast<void>(
        pg::ConnectionLocator::parse("password='very-secret' not_a_libpq_option=value"));
    FAIL("invalid locator was accepted");
  } catch (const dbdiff::Error& error) {
    CHECK(error.code() == dbdiff::ErrorCode::configuration);
    CHECK(std::string_view{error.what()}.find("very-secret") == std::string_view::npos);
  }

  const std::string locator_with_nul{"host=localhost\0password=secret", 30};
  CHECK_THROWS_AS(pg::ConnectionLocator::parse(locator_with_nul), dbdiff::Error);
  CHECK_THROWS_AS(pg::ConnectionLocator::parse("host=localhost").with_database(""), dbdiff::Error);
}

TEST_CASE("PostgreSQL server versions 15 through 18 are accepted", "[unit][postgresql]") {
  CHECK(pg::validate_server_version(150000) == pg::ServerVersion{150000, 15, 0});
  CHECK(pg::parse_server_version("150017") == pg::ServerVersion{150017, 15, 17});
  CHECK(pg::parse_server_version("180004") == pg::ServerVersion{180004, 18, 4});
  CHECK(pg::validate_server_version(189999).major == 18);

  CHECK_THROWS_AS(pg::validate_server_version(149999), dbdiff::Error);
  CHECK_THROWS_AS(pg::validate_server_version(190000), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_server_version("18.1"), dbdiff::Error);
  CHECK_THROWS_AS(pg::parse_server_version(""), dbdiff::Error);
}

TEST_CASE("PostgreSQL SQL quoting handles identifiers, literals, and control bytes",
          "[unit][postgresql]") {
  CHECK(pg::quote_identifier("odd\"name") == "\"odd\"\"name\"");
  CHECK(pg::quote_literal("a'b\\c\n") == "E'a\\'b\\\\c\\n'");
  CHECK(pg::quote_literal(std::string{"\x01", 1}) == "E'\\001'");

  CHECK_THROWS_AS(pg::quote_identifier(""), dbdiff::Error);
  CHECK_THROWS_AS(pg::quote_identifier(std::string{"a\0b", 3}), dbdiff::Error);
  CHECK_THROWS_AS(pg::quote_literal(std::string{"a\0b", 3}), dbdiff::Error);
}

TEST_CASE("PostgreSQL scratch names and markers are safe value objects", "[unit][postgresql]") {
  constexpr std::string_view token = "0123456789abcdef0123456789abcdef";
  const auto identity = pg::ScratchIdentity::from_token(token, 123456789);

  CHECK(identity.name.value() == "dbdiff_0123456789abcdef0123456789abcdef");
  CHECK(identity.name.token() == token);
  CHECK(identity.name.quoted() == "\"dbdiff_0123456789abcdef0123456789abcdef\"");
  CHECK(identity.marker.value() == "dbdiff:scratch:v1:0123456789abcdef0123456789abcdef:123456789");
  CHECK(identity.marker.quoted() ==
        "E'dbdiff:scratch:v1:0123456789abcdef0123456789abcdef:123456789'");

  const auto generated = pg::ScratchIdentity::generate();
  CHECK(generated.name.value().starts_with("dbdiff_"));
  CHECK(generated.name.token().size() == 32);
  for (const char character : generated.name.token()) {
    CHECK(((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')));
  }

  CHECK_THROWS_AS(pg::ScratchIdentity::from_token("ABCDEF", 0), dbdiff::Error);
  CHECK_THROWS_AS(pg::ScratchIdentity::from_token(token, -1), dbdiff::Error);
}

TEST_CASE("PostgreSQL snapshots normalize without retaining catalog OIDs", "[unit][postgresql]") {
  pg::SchemaSnapshot snapshot{
      .server_version = pg::ServerVersion{150017, 0, 0},
      .schemas = {"utils", "public"},
      .tables =
          {
              pg::Table{
                  .name = {"utils", "zeta"},
                  .persistence = pg::TablePersistence::unlogged,
                  .columns =
                      {
                          pg::Column{.position = 2, .name = "payload", .type = "text"},
                          pg::Column{
                              .position = 1, .name = "id", .type = "bigint", .not_null = true},
                      },
              },
              pg::Table{
                  .name = {"public", "alpha"},
                  .columns =
                      {
                          pg::Column{
                              .position = 1,
                              .name = "id",
                              .type = "integer",
                              .not_null = true,
                              .identity = pg::IdentityGeneration::always,
                          },
                      },
              },
          },
  };

  const auto normalized = pg::normalize_snapshot(std::move(snapshot));
  CHECK(normalized.server_version == pg::ServerVersion{150017, 15, 17});
  REQUIRE(normalized.schemas.size() == 2);
  CHECK(normalized.schemas[0] == "public");
  CHECK(normalized.schemas[1] == "utils");
  REQUIRE(normalized.tables.size() == 2);
  CHECK(normalized.tables[0].name == pg::QualifiedName{"public", "alpha"});
  CHECK(normalized.tables[1].name == pg::QualifiedName{"utils", "zeta"});
  REQUIRE(normalized.tables[1].columns.size() == 2);
  CHECK(normalized.tables[1].columns[0].name == "id");
  CHECK(normalized.tables[1].columns[1].name == "payload");
}

TEST_CASE("PostgreSQL snapshot normalization rejects ambiguous catalog state",
          "[unit][postgresql]") {
  const auto version = pg::ServerVersion{150000, 15, 0};

  SECTION("duplicate schema") {
    CHECK_THROWS_AS(pg::normalize_snapshot({version, {"public", "public"}, {}}), dbdiff::Error);
  }
  SECTION("metadata schema") {
    CHECK_THROWS_AS(pg::normalize_snapshot({version, {"_dbdiff"}, {}}), dbdiff::Error);
  }
  SECTION("table in unknown schema") {
    CHECK_THROWS_AS(
        pg::normalize_snapshot({version, {"public"}, {pg::Table{.name = {"other", "t"}}}}),
        dbdiff::Error);
  }
  SECTION("duplicate column position") {
    pg::Table table{
        .name = {"public", "t"},
        .columns =
            {
                pg::Column{.position = 1, .name = "a", .type = "integer"},
                pg::Column{.position = 1, .name = "b", .type = "integer"},
            },
    };
    CHECK_THROWS_AS(pg::normalize_snapshot({version, {"public"}, {std::move(table)}}),
                    dbdiff::Error);
  }
  SECTION("generated column without expression") {
    pg::Table table{
        .name = {"public", "t"},
        .columns =
            {
                pg::Column{
                    .position = 1,
                    .name = "computed",
                    .type = "integer",
                    .generated = pg::GeneratedStorage::stored,
                },
            },
    };
    CHECK_THROWS_AS(pg::normalize_snapshot({version, {"public"}, {std::move(table)}}),
                    dbdiff::Error);
  }
  SECTION("identity column with separate default") {
    pg::Table table{
        .name = {"public", "t"},
        .columns =
            {
                pg::Column{
                    .position = 1,
                    .name = "id",
                    .type = "integer",
                    .default_expression = "nextval('other')",
                    .identity = pg::IdentityGeneration::always,
                },
            },
    };
    CHECK_THROWS_AS(pg::normalize_snapshot({version, {"public"}, {std::move(table)}}),
                    dbdiff::Error);
  }
}
