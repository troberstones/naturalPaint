#pragma once

// flats/FlatsSelfTest -- the flats section of `--selftest`, declared here
// rather than only in app/SelfTest.hpp because it is the one section that
// must also link into a binary with no SDL, no wgpu and no ImGui
// (src/tools/FlatsTestMain.cpp). app/SelfTest.hpp re-declares it so the
// single public index stays complete.

namespace np {

bool runFlatsTest();

}  // namespace np
