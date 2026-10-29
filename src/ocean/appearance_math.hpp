#pragma once
// ----------------------------------------------------------------------------
// appearance_math — the double-precision transcendentals the appearance
// genome's construction uses (appearance_model.hpp), vendored so that native
// and wasm evaluate them identically, and identically to the accepted
// prototype (gallery v3, which computes in JS doubles and rounds each stored
// gene to binary32).
//
//   cos, sin   musl's (FreeBSD msun / fdlibm lineage: cos.c, sin.c, __cos.c,
//              __sin.c and the medium path of __rem_pio2.c), verbatim apart
//              from the dropped FORCE_EVAL / errno lines (as in
//              src/core/portable_math.hpp) and the large-argument reduction
//              (__rem_pio2_large), which nothing here can reach: every call is
//              with |x| < 2^20·π/2 ≈ 1.6e6 (construction: |x| <= 2π; the
//              reference evaluator: |x| < 1e4). Outside that domain they
//              return NaN rather than an inaccurate value.
//   atan2      musl's atan2.c + atan.c, verbatim (minus FORCE_EVAL).
//   cubeLattice(x)  x³ correctly rounded, for x a multiple of 2^-24 in
//              [-1, 1] (the M1 founder transform N(r) = .5 + .35(2r − 1)³ on a
//              draw r = k·2^-24): the cube of the 25-bit integer is exact in
//              128 bits, and the 128-bit → double conversion rounds once.
//
// Why these and not the C library: glibc and musl disagree in the last bit of
// sin / cos on a large share of arguments (and glibc dispatches on the CPU),
// so a libm call would make native != wasm. V8 (the gallery's engine) does
// not use musl's sin / cos either, and its Math.pow(x, 3) differs between
// Node versions — but every gene is rounded to binary32, and over every
// draw value the gallery's formulas can see (2^24 per formula; the chroma and
// orientation encodings, the warp square, the N(r) cube, the cross-pair
// mean's cos / sin / atan2) these functions give V8's binary32 result in
// every case (measured 2026-10-28 against Node 26.1 and Node 20.20; probe
// scripts in the appearance PR). The construction's other operations are
// IEEE-exact: + − × ÷, sqrt, floor.
//
// Pure double arithmetic: with -ffp-contract=off (the Ocean builds' flag) it
// evaluates the same on every IEEE-754 target. Ocean-only (Classic never
// includes src/ocean).
// ----------------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace appmath {
namespace detail {
inline std::uint64_t bits(double x) { std::uint64_t u; std::memcpy(&u, &x, 8); return u; }
inline double fromBits(std::uint64_t u) { double x; std::memcpy(&x, &u, 8); return x; }
inline std::uint32_t highWord(double x) { return (std::uint32_t)(bits(x) >> 32); }

// __cos.c
inline double kcos(double x, double y) {
    const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                 C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                 C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    double hz, z, r, w;
    z = x * x;
    w = z * z;
    r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    hz = 0.5 * z;
    w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}
// __sin.c
inline double ksin(double x, double y, int iy) {
    const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                 S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                 S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    double z, r, v, w;
    z = x * x;
    w = z * z;
    r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
    v = z * x;
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}
// __rem_pio2.c for |x| < 2^20·π/2 (ix < 0x413921fb); returns n, y[0] + y[1] = x − n·π/2.
inline int remPio2(double x, double* y) {
    const double toint = 1.5 / 2.22044604925031308085e-16,   // 1.5 / DBL_EPSILON
                 pio4 = 0x1.921fb54442d18p-1,
                 invpio2 = 6.36619772367581382433e-01,
                 pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
                 pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
                 pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32;
    std::uint64_t ui = bits(x);
    double z, w, t, r, fn;
    std::uint32_t ix;
    int sign, n, ex, ey;
    sign = (int)(ui >> 63);
    ix = (std::uint32_t)(ui >> 32) & 0x7fffffff;
    if (ix <= 0x400f6a7a) {   /* |x| ~<= 5pi/4 */
        if ((ix & 0xfffff) == 0x921fb) goto medium;   /* |x| ~= pi/2 or 2pi/2 */
        if (ix <= 0x4002d97c) {   /* |x| ~<= 3pi/4 */
            if (!sign) { z = x - pio2_1; y[0] = z - pio2_1t; y[1] = (z - y[0]) - pio2_1t; return 1; }
            else       { z = x + pio2_1; y[0] = z + pio2_1t; y[1] = (z - y[0]) + pio2_1t; return -1; }
        } else {
            if (!sign) { z = x - 2 * pio2_1; y[0] = z - 2 * pio2_1t; y[1] = (z - y[0]) - 2 * pio2_1t; return 2; }
            else       { z = x + 2 * pio2_1; y[0] = z + 2 * pio2_1t; y[1] = (z - y[0]) + 2 * pio2_1t; return -2; }
        }
    }
    if (ix <= 0x401c463b) {   /* |x| ~<= 9pi/4 */
        if (ix <= 0x4015fdbc) {   /* |x| ~<= 7pi/4 */
            if (ix == 0x4012d97c) goto medium;   /* |x| ~= 3pi/2 */
            if (!sign) { z = x - 3 * pio2_1; y[0] = z - 3 * pio2_1t; y[1] = (z - y[0]) - 3 * pio2_1t; return 3; }
            else       { z = x + 3 * pio2_1; y[0] = z + 3 * pio2_1t; y[1] = (z - y[0]) + 3 * pio2_1t; return -3; }
        } else {
            if (ix == 0x401921fb) goto medium;   /* |x| ~= 4pi/2 */
            if (!sign) { z = x - 4 * pio2_1; y[0] = z - 4 * pio2_1t; y[1] = (z - y[0]) - 4 * pio2_1t; return 4; }
            else       { z = x + 4 * pio2_1; y[0] = z + 4 * pio2_1t; y[1] = (z - y[0]) + 4 * pio2_1t; return -4; }
        }
    }
medium:
    /* rint(x/(pi/2)) */
    fn = x * invpio2 + toint - toint;
    n = (std::int32_t)fn;
    r = x - fn * pio2_1;
    w = fn * pio2_1t;   /* 1st round, good to 85 bits */
    if (r - w < -pio4) {   /* Matters with directed rounding. */
        n--; fn--;
        r = x - fn * pio2_1;
        w = fn * pio2_1t;
    } else if (r - w > pio4) {
        n++; fn++;
        r = x - fn * pio2_1;
        w = fn * pio2_1t;
    }
    y[0] = r - w;
    ey = (int)(bits(y[0]) >> 52 & 0x7ff);
    ex = (int)(ix >> 20);
    if (ex - ey > 16) {   /* 2nd round, good to 118 bits */
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        ey = (int)(bits(y[0]) >> 52 & 0x7ff);
        if (ex - ey > 49) {   /* 3rd round, good to 151 bits, covers all cases */
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y[0] = r - w;
        }
    }
    y[1] = (r - y[0]) - w;
    return n;
}
inline constexpr std::uint32_t kMaxHigh = 0x413921fb;   // |x| < 2^20·π/2: no large reduction needed
}  // namespace detail

inline double cos(double x) {
    using namespace detail;
    double y[2];
    std::uint32_t ix = highWord(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {   /* |x| ~< pi/4 */
        if (ix < 0x3e46a09e) return 1.0;   /* |x| < 2**-27 * sqrt(2) */
        return kcos(x, 0);
    }
    if (ix >= kMaxHigh) return std::numeric_limits<double>::quiet_NaN();   // outside the domain (and inf / NaN)
    unsigned n = (unsigned)remPio2(x, y);
    switch (n & 3) {
    case 0: return kcos(y[0], y[1]);
    case 1: return -ksin(y[0], y[1], 1);
    case 2: return -kcos(y[0], y[1]);
    default: return ksin(y[0], y[1], 1);
    }
}

inline double sin(double x) {
    using namespace detail;
    double y[2];
    std::uint32_t ix = highWord(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {   /* |x| ~< pi/4 */
        if (ix < 0x3e500000) return x;   /* |x| < 2**-26 */
        return ksin(x, 0.0, 0);
    }
    if (ix >= kMaxHigh) return std::numeric_limits<double>::quiet_NaN();
    unsigned n = (unsigned)remPio2(x, y);
    switch (n & 3) {
    case 0: return ksin(y[0], y[1], 1);
    case 1: return kcos(y[0], y[1]);
    case 2: return -ksin(y[0], y[1], 1);
    default: return -kcos(y[0], y[1]);
    }
}

// atan.c
inline double atan(double x) {
    static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                    9.82793723247329054082e-01, 1.57079632679489655800e+00};
    static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                    1.39033110312309984516e-17, 6.12323399573676603587e-17};
    static const double aT[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01,
                                1.42857142725034663711e-01, -1.11111104054623557880e-01,
                                9.09088713343650656196e-02, -7.69187620504482999495e-02,
                                6.66107313738753120669e-02, -5.83357013379057348645e-02,
                                4.97687799461593236017e-02, -3.65315727442169155270e-02,
                                1.62858201153657823623e-02};
    double w, s1, s2, z;
    std::uint32_t ix = detail::highWord(x), sign = ix >> 31;
    int id;
    ix &= 0x7fffffff;
    if (ix >= 0x44100000) {   /* if |x| >= 2^66 */
        if (std::isnan(x)) return x;
        z = atanhi[3] + 0x1p-120f;
        return sign ? -z : z;
    }
    if (ix < 0x3fdc0000) {   /* |x| < 0.4375 */
        if (ix < 0x3e400000) return x;   /* |x| < 2^-27 */
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3ff30000) {   /* |x| < 1.1875 */
            if (ix < 0x3fe60000) { id = 0; x = (2.0 * x - 1.0) / (2.0 + x); }   /*  7/16 <= |x| < 11/16 */
            else                 { id = 1; x = (x - 1.0) / (x + 1.0); }       /* 11/16 <= |x| < 19/16 */
        } else {
            if (ix < 0x40038000) { id = 2; x = (x - 1.5) / (1.0 + 1.5 * x); }  /* |x| < 2.4375 */
            else                 { id = 3; x = -1.0 / x; }                    /* 2.4375 <= |x| < 2^66 */
        }
    }
    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - (x * (s1 + s2) - atanlo[id] - x);
    return sign ? -z : z;
}

// atan2.c
inline double atan2(double y, double x) {
    const double pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    double z;
    std::uint32_t m, lx, ly, ix, iy;
    if (std::isnan(x) || std::isnan(y)) return x + y;
    const std::uint64_t ux = detail::bits(x), uy = detail::bits(y);
    ix = (std::uint32_t)(ux >> 32); lx = (std::uint32_t)ux;
    iy = (std::uint32_t)(uy >> 32); ly = (std::uint32_t)uy;
    if (((ix - 0x3ff00000) | lx) == 0) return atan(y);   /* x = 1.0 */
    m = ((iy >> 31) & 1) | ((ix >> 30) & 2);             /* 2*sign(x)+sign(y) */
    ix = ix & 0x7fffffff;
    iy = iy & 0x7fffffff;
    if ((iy | ly) == 0) {   /* when y = 0 */
        switch (m) {
        case 0:
        case 1: return y;
        case 2: return pi;
        default: return -pi;
        }
    }
    if ((ix | lx) == 0) return (m & 1) ? -pi / 2 : pi / 2;   /* when x = 0 */
    if (ix == 0x7ff00000) {   /* when x is INF */
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0: return pi / 4;
            case 1: return -pi / 4;
            case 2: return 3 * pi / 4;
            default: return -3 * pi / 4;
            }
        } else {
            switch (m) {
            case 0: return 0.0;
            case 1: return -0.0;
            case 2: return pi;
            default: return -pi;
            }
        }
    }
    if (ix + (64 << 20) < iy || iy == 0x7ff00000) return (m & 1) ? -pi / 2 : pi / 2;   /* |y/x| > 0x1p64 */
    if ((m & 2) && iy + (64 << 20) < ix) z = 0;   /* |y/x| < 0x1p-64, x<0 */
    else z = atan(std::fabs(y / x));
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

// x³, correctly rounded, for x = k·2^-24 with |k| <= 2^24 (exactly representable).
inline double cubeLattice(double x) {
    const long long m = (long long)(x * 16777216.0);   // exact: x is on the 2^-24 lattice
    const __int128 c = (__int128)m * m * m;            // |c| <= 2^72: exact
    return std::ldexp((double)c, -72);                 // one rounding (the conversion), then exact scaling
}
}  // namespace appmath
