#include "dbdiff/cli.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
  return dbdiff::run_cli(argc, argv, std::cout, std::cerr, std::filesystem::current_path());
}
