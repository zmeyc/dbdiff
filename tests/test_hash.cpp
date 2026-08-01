#include "dbdiff/error.hpp"
#include "dbdiff/hash.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("SHA-256 produces stable hexadecimal digests", "[unit][SRC-004]") {
  CHECK(dbdiff::sha256_hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(dbdiff::sha256_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("length-prefixed hashing distinguishes field boundaries", "[unit][SRC-004]") {
  dbdiff::Sha256 first;
  first.add_length_prefixed("ab");
  first.add_length_prefixed("c");

  dbdiff::Sha256 second;
  second.add_length_prefixed("a");
  second.add_length_prefixed("bc");

  CHECK(first.finish_hex() != second.finish_hex());
}

TEST_CASE("finished hashes reject further use", "[unit][SRC-004]") {
  dbdiff::Sha256 digest;
  digest.add("value");
  static_cast<void>(digest.finish_hex());
  CHECK_THROWS_AS(digest.add("other"), dbdiff::Error);
  CHECK_THROWS_AS(digest.finish_hex(), dbdiff::Error);
}
