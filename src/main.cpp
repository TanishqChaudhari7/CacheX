#include <iostream>

#include "cachex/version.hpp"

int main() {
  std::cout << "CacheX " << cachex::version_string() << "\n"
            << "Foundation build: no cache engine and no server yet.\n";
  return 0;
}
