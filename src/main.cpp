#include "dbdiff/version.hpp"

#include <CLI/CLI.hpp>

#include <iostream>

int main(int argc, char** argv) {
  CLI::App app{"Create and apply deterministic database schema migrations", "dbdiff"};
  app.set_version_flag("--version", std::string{dbdiff::version()});

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    return app.exit(error);
  }

  std::cout << app.help();
  return 0;
}
