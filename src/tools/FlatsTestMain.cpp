// flatstest -- the flats self-test section as a standalone, GPU-free binary.
//
// The main `--selftest` driver creates a window and a WebGPU adapter before it
// reaches any section, so on a machine with neither (a Linux CI box, or the
// container src/flats/ was ported in) no CPU section can run. src/flats/ has
// no GPU dependency at all, and this executable is how it is exercised there:
// `goldentool`'s shape -- a second add_executable listing only what it needs.
//
// Exit code 0 on pass, 1 on any failed check.

#include <cstdio>

#include "flats/FlatsSelfTest.hpp"

int main() {
  const bool ok = np::runFlatsTest();
  std::printf("%s\n", ok ? "flatstest: PASS" : "flatstest: FAIL");
  return ok ? 0 : 1;
}
