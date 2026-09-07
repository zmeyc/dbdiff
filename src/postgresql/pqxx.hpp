#pragma once

#if defined(DBDIFF_PQXX_DISABLE_SOURCE_LOCATION)
// libpqxx 7.8 derives its exception ABI from the consumer's C++ feature macros,
// even when the installed library was built as C++17. CMake enables this shim
// only after checking that its constructors link against that library.
#include <version>
#pragma push_macro("__cpp_lib_source_location")
#undef __cpp_lib_source_location
#include <pqxx/pqxx>
#pragma pop_macro("__cpp_lib_source_location")

#if !defined(pqxx_have_source_location) || pqxx_have_source_location
#error "This libpqxx version does not support the detected legacy exception ABI"
#endif
#else
#include <pqxx/pqxx>
#endif
