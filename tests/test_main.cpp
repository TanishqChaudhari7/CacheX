#include "test_framework.hpp"

// Every test file registers itself, so main() never needs to know which tests
// exist. Adding a test file to tests/CMakeLists.txt is the only wiring needed.
int main() { return cachex::test::run_all(); }
