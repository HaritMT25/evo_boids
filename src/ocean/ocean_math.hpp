#pragma once
// ----------------------------------------------------------------------------
// ocean_math — Ocean-local portable math, built only from evomath's portable
// primitives (src/core/portable_math.hpp, the same code in every build) and
// IEEE +, −, ×, ÷ (correctly rounded everywhere; the builds use
// -ffp-contract=off), so native and wasm give the same bits. src/core stays
// untouched: Classic is byte-identical.
// ----------------------------------------------------------------------------
#include "../core/portable_math.hpp"

namespace oceanmath {

// e^x, from evomath::expm1f:
//   x > 0:  expm1f(x) + 1         (one rounding on top of expm1f's)
//   x <= 0: 1 / (expm1f(−x) + 1)  (e^x as 1 / e^{−x})
// Both keep full RELATIVE precision (within ~2 ulp of e^x) over the whole
// float range. 1 + expm1f(x) for x < 0 does not: it keeps only ~6e-8
// ABSOLUTE, so its relative error grows with depth and it is exactly 0 for
// x < −17.33 (e^{−18} = 1.5e-8 is representable). Here e^x is 0 only once
// e^{−x} overflows a float (x < −88.72, where e^x < 3e-39 is subnormal),
// and inf only once expm1f(x) does (x > 88.72). expf(0) == expf(−0) == 1.
inline float expf(float x) {
    if (x > 0.0f) return evomath::expm1f(x) + 1.0f;
    return 1.0f / (evomath::expm1f(-x) + 1.0f);
}

}  // namespace oceanmath
