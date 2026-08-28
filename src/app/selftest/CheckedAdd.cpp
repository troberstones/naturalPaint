#include "app/selftest/Support.hpp"

#include "io/AbrBrushes.hpp"

namespace np {

// docs/architecture-review.md P2-2 item 1: io/AbrBrushes.hpp's `checkedAdd()`
// replaced every `if (at + n > buf.size())`-shaped bounds check in
// io/AbrBrushes.cpp with one helper that refuses instead of wrapping. The
// parser call sites all stay well clear of SIZE_MAX -- every offset they pass
// in is itself the result of an earlier `<= buf.size()` check -- so none of
// them exercises the wrap this function exists to catch. This section does,
// directly, at the boundary that matters: `SIZE_MAX` and `SIZE_MAX - 1`,
// where the naive `a + b > limit` idiom this replaces would silently wrap to
// a small number and pass a check it should have failed. A helper nothing
// drives at that boundary is not evidence the boundary is handled; this is
// that evidence. Headless and GPU-free -- pure CPU, no PaintSim involvement.
bool runCheckedAddTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr size_t kMax = std::numeric_limits<size_t>::max();

  // --- the exact wrap the old idiom missed: `SIZE_MAX + 1 > buf.size()`
  // wraps to `0 > buf.size()`, which is false for any real buffer, so a
  // hostile or merely mis-derived `at == SIZE_MAX` would have sailed through
  // `if (at + 1 > b.size())` as though it were in range. checkedAdd refuses
  // outright instead. ---
  {
    size_t out = 12345;  // sentinel: must be left untouched on refusal
    const bool r = checkedAdd(kMax, 1, out);
    check(!r, "checkedAdd(SIZE_MAX, 1): refuses rather than wrapping to 0");
    check(out == 12345, "checkedAdd(SIZE_MAX, 1): out untouched on refusal");
  }

  // --- one below the wrap: SIZE_MAX - 1 + 1 == SIZE_MAX exactly, the last
  // sum that still fits. The boundary a helper this simple could get wrong
  // by one either direction (refusing a sum that fits, or accepting one that
  // doesn't) is exactly this one. ---
  {
    size_t out = 0;
    const bool r = checkedAdd(kMax - 1, 1, out);
    check(r, "checkedAdd(SIZE_MAX - 1, 1): the last sum that still fits succeeds");
    check(out == kMax, "checkedAdd(SIZE_MAX - 1, 1): sums to exactly SIZE_MAX");
  }

  // --- symmetric on the other argument, since the guard is `a > max() - b`
  // and swapping which side carries the large value is a plausible way to
  // get the comparison backwards. ---
  {
    size_t out = 0;
    const bool r = checkedAdd(1, kMax - 1, out);
    check(r, "checkedAdd(1, SIZE_MAX - 1): same boundary, arguments swapped");
    check(out == kMax, "checkedAdd(1, SIZE_MAX - 1): sums to exactly SIZE_MAX");
  }

  // --- an ordinary case, so the helper is proven to compute the right
  // answer and not merely to refuse everything near the boundary. ---
  {
    size_t out = 0;
    const bool r = checkedAdd(37, 5, out);
    check(r && out == 42, "checkedAdd(37, 5): an ordinary sum succeeds and is exact");
  }

  // --- b == 0 at the very top of the range: SIZE_MAX + 0 == SIZE_MAX, which
  // must succeed -- the guard is `a > max() - b`, and `max() - 0 == max()`,
  // so this is the one case at this boundary that should NOT be refused. ---
  {
    size_t out = 0;
    const bool r = checkedAdd(kMax, 0, out);
    check(r && out == kMax, "checkedAdd(SIZE_MAX, 0): adding zero at the top still succeeds");
  }

  std::printf("[selftest] checked-add %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
