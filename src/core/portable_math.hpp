#pragma once
#include <cstdint>
#include <cstring>
#include <cfloat>
#include <cmath>

// ============================================================
// Portable transcendentals for cross-toolchain, cross-CPU bit-exactness.
//
// The sim calls tanh/sin/cos/atan2/exp/log. glibc (native builds) and musl
// (Emscripten/wasm builds) implement these with different algorithms, and
// none of them are correctly-rounded, so the two libms disagree by an ulp
// on some inputs — enough to diverge a chaotic system. Measured 2026-07-25
// (glibc 2.39 vs emsdk 6.0.4): tanhf, sinf, cosf, atan2f and expm1f differ.
//
// expf and logf looked bit-identical then, but only on one CPU's code path:
// glibc picks expf/logf at load time (an ifunc), an FMA build on CPUs with
// FMA/AVX2 and a generic one otherwise, so a native run depended on the CPU
// it ran on. musl runs the same Arm optimized-routines code without FMA,
// i.e. glibc's generic path. Measured 2026-10-28, glibc 2.39 on an FMA/AVX2
// CPU: expf(-0x1.f8cbb2p+5) = 0x11fa2993 with default dispatch, 0x11fa2992
// with GLIBC_TUNABLES=glibc.cpu.hwcaps=-AVX2_Usable,-FMA_Usable,-AVX2,-FMA
// and in musl. Over all 2^32 inputs the FMA expf differs from musl on
// exactly two (0x4202422f and 0xc27c65d9, 1 ulp each); logf on none. Double
// tanh (the genome colour) differs between glibc and musl by up to 3 ulp on
// about half of all inputs in [0.25, 1), 27% of real colour arguments, and
// glibc's own result there also depends on the dispatch. sqrt is IEEE-exact
// everywhere and stays on the library.
//
// The fix: both builds compile THIS source instead of calling libm. The
// implementations are musl's (fdlibm and Arm optimized-routines lineage,
// taken verbatim from the musl copy bundled with emsdk 6.0.4), which are
// pure IEEE float/double arithmetic — with -ffp-contract=off they evaluate
// identically on every IEEE-754 target and CPU. Vendored at v0.18.0: tanhf,
// sinf, cosf, atanf, atan2f, expm1f (switching native code from glibc to
// these shifted trajectories once — a numerical-lineage break, same framing
// as -ffp-contract=off in 0.16.0; see the research log). Vendored
// 2026-10-28: expf, logf and double tanh (with its double expm1). Being
// musl's own code they leave every wasm result exactly as it was; natively
// they equal glibc's generic path, so native results no longer depend on
// the CPU. Checked over all 2^32 floats (expf, logf: native == wasm ==
// musl == glibc generic) and on 2.4e8 stratified doubles plus every
// branch threshold and real colour inputs (tanh: native == wasm == musl).
//
// Dropped from musl: errno and the fp_barrier / FORCE_EVAL calls, which only
// keep the compiler from evaluating an operation at compile time (or not at
// all) so that its FP exception flag is raised at run time. Without them the
// compiler may fold, e.g., the overflow / underflow products (0x1p97f *
// 0x1p97f) into constants, and a folded constant is the round-to-nearest
// result. So this copy assumes the IEEE defaults the simulation always runs
// with: round-to-nearest-even and gradual underflow (no flush-to-zero /
// denormals-are-zero). The simulation never changes the rounding mode and
// never reads the FP flags.
//
// Licences: musl MIT, Arm optimized-routines MIT, fdlibm (Sun) as noted.
// Original notices preserved below; see THIRD_PARTY_NOTICES.md.
// ============================================================

static_assert(FLT_EVAL_METHOD == 0,
              "portable_math assumes float_t==float / double_t==double "
              "(true on x86-64 SSE and wasm32)");

namespace evomath {
namespace detail {

inline uint32_t floatBits(float x) { uint32_t u; std::memcpy(&u, &x, 4); return u; }
inline float bitsFloat(uint32_t u) { float x; std::memcpy(&x, &u, 4); return x; }
inline bool predictFalse(bool b) { return __builtin_expect(b, 0); }

// musl's FORCE_EVAL only raises FP exception flags (inexact/underflow);
// it never changes a computed value and the sim never reads FP flags.
template <typename T> inline void forceEval(T) {}

constexpr double kPiOver2 = 1.57079632679489661923;

// ---- expm1f (origin: FreeBSD s_expm1f.c, via musl) ----
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
inline float expm1f_impl(float x)
{
    static const float
    ln2_hi = 6.9313812256e-01f, /* 0x3f317180 */
    ln2_lo = 9.0580006145e-06f, /* 0x3717f7d1 */
    invln2 = 1.4426950216e+00f, /* 0x3fb8aa3b */
    Q1 = -3.3333212137e-2f,     /* -0x888868.0p-28 */
    Q2 =  1.5807170421e-3f;     /*  0xcf3010.0p-33 */

    float y, hi, lo, c = 0, t, e, hxs, hfx, r1, twopk;
    union { float f; uint32_t i; } u = {x};
    uint32_t hx = u.i & 0x7fffffff;
    int k, sign = u.i >> 31;

    /* filter out huge and non-finite argument */
    if (hx >= 0x4195b844) {  /* if |x|>=27*ln2 */
        if (hx > 0x7f800000)  /* NaN */
            return x;
        if (sign)
            return -1;
        if (hx > 0x42b17217) { /* x > log(FLT_MAX) */
            x *= 0x1p127f;
            return x;
        }
    }

    /* argument reduction */
    if (hx > 0x3eb17218) {           /* if  |x| > 0.5 ln2 */
        if (hx < 0x3F851592) {       /* and |x| < 1.5 ln2 */
            if (!sign) {
                hi = x - ln2_hi;
                lo = ln2_lo;
                k = 1;
            } else {
                hi = x + ln2_hi;
                lo = -ln2_lo;
                k = -1;
            }
        } else {
            k  = invln2*x + (sign ? -0.5f : 0.5f);
            t  = k;
            hi = x - t*ln2_hi;      /* t*ln2_hi is exact here */
            lo = t*ln2_lo;
        }
        x = hi-lo;
        c = (hi-x)-lo;
    } else if (hx < 0x33000000) {  /* when |x|<2**-25, return x */
        if (hx < 0x00800000)
            forceEval(x*x);
        return x;
    } else
        k = 0;

    /* x is now in primary range */
    hfx = 0.5f*x;
    hxs = x*hfx;
    r1 = 1.0f+hxs*(Q1+hxs*Q2);
    t  = 3.0f - r1*hfx;
    e  = hxs*((r1-t)/(6.0f - x*t));
    if (k == 0)  /* c is 0 */
        return x - (x*e-hxs);
    e  = x*(e-c) - c;
    e -= hxs;
    /* exp(x) ~ 2^k (x_reduced - e + 1) */
    if (k == -1)
        return 0.5f*(x-e) - 0.5f;
    if (k == 1) {
        if (x < -0.25f)
            return -2.0f*(e-(x+0.5f));
        return 1.0f + 2.0f*(x-e);
    }
    u.i = (0x7f+k)<<23;  /* 2^k */
    twopk = u.f;
    if (k < 0 || k > 56) {   /* suffice to return exp(x)-1 */
        y = x - e + 1.0f;
        if (k == 128)
            y = y*2.0f*0x1p127f;
        else
            y = y*twopk;
        return y - 1.0f;
    }
    u.i = (0x7f-k)<<23;  /* 2^-k */
    if (k < 23)
        y = (x-e+(1-u.f))*twopk;
    else
        y = (x-(e+u.f)+1)*twopk;
    return y;
}

// ---- kernel sin/cos on a double-reduced argument
//      (origin: FreeBSD k_sinf.c / k_cosf.c, via musl) ----
inline float sindf(double x)
{
    /* |sin(x)/x - s(x)| < 2**-37.5 (~[-4.89e-12, 4.824e-12]). */
    static const double
    S1 = -0x15555554cbac77.0p-55,
    S2 =  0x111110896efbb2.0p-59,
    S3 = -0x1a00f9e2cae774.0p-65,
    S4 =  0x16cd878c3b46a7.0p-71;

    double r, s, w, z;
    z = x*x;
    w = z*z;
    r = S3 + z*S4;
    s = z*x;
    return (float)((x + s*(S1 + z*S2)) + s*w*r);
}

inline float cosdf(double x)
{
    /* |cos(x) - c(x)| < 2**-34.1 (~[-5.37e-11, 5.295e-11]). */
    static const double
    C0 = -0x1ffffffd0c5e81.0p-54,
    C1 =  0x155553e1053a42.0p-57,
    C2 = -0x16c087e80f1e27.0p-62,
    C3 =  0x199342e0ee5069.0p-68;

    double r, w, z;
    z = x*x;
    w = z*z;
    r = C2+z*C3;
    return (float)(((1.0+z*C0) + w*C1) + (w*z)*r);
}

// ---- __rem_pio2_large (origin: FreeBSD k_rem_pio2.c, via musl) ----
// Only reached for |x| >= 2^28*(pi/2) ~ 4.2e8 — far beyond anything the sim
// produces — but vendored verbatim so the huge-argument path is identical
// too. Uses std::scalbn/std::floor, which are IEEE-exact everywhere.
inline int remPio2Large(double *x, double *y, int e0, int nx, int prec)
{
    static const int init_jk[] = {3,4,4,6};
    static const int32_t ipio2[] = {
    0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62,
    0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A,
    0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129,
    0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41,
    0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8,
    0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF,
    0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5,
    0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08,
    0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3,
    0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880,
    0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B,
    };
    static const double PIo2[] = {
      1.57079625129699707031e+00,
      7.54978941586159635335e-08,
      5.39030252995776476554e-15,
      3.28200341580791294123e-22,
      1.27065575308067607349e-29,
      1.22933308981111328932e-36,
      2.73370053816464559624e-44,
      2.16741683877804819444e-51,
    };

    int32_t jz,jx,jv,jp,jk,carry,n,iq[20],i,j,k,m,q0,ih;
    double z,fw,f[20],fq[20],q[20];

    jk = init_jk[prec];
    jp = jk;

    jx = nx-1;
    jv = (e0-3)/24;  if(jv<0) jv=0;
    q0 = e0-24*(jv+1);

    j = jv-jx; m = jx+jk;
    for (i=0; i<=m; i++,j++)
        f[i] = j<0 ? 0.0 : (double)ipio2[j];

    for (i=0; i<=jk; i++) {
        for (j=0,fw=0.0; j<=jx; j++)
            fw += x[j]*f[jx+i-j];
        q[i] = fw;
    }

    jz = jk;
recompute:
    for (i=0,j=jz,z=q[jz]; j>0; i++,j--) {
        fw    = (double)(int32_t)(0x1p-24*z);
        iq[i] = (int32_t)(z - 0x1p24*fw);
        z     = q[j-1]+fw;
    }

    z  = std::scalbn(z,q0);
    z -= 8.0*std::floor(z*0.125);
    n  = (int32_t)z;
    z -= (double)n;
    ih = 0;
    if (q0 > 0) {
        i  = iq[jz-1]>>(24-q0); n += i;
        iq[jz-1] -= i<<(24-q0);
        ih = iq[jz-1]>>(23-q0);
    }
    else if (q0 == 0) ih = iq[jz-1]>>23;
    else if (z >= 0.5) ih = 2;

    if (ih > 0) {
        n += 1; carry = 0;
        for (i=0; i<jz; i++) {
            j = iq[i];
            if (carry == 0) {
                if (j != 0) {
                    carry = 1;
                    iq[i] = 0x1000000 - j;
                }
            } else
                iq[i] = 0xffffff - j;
        }
        if (q0 > 0) {
            switch(q0) {
            case 1:
                iq[jz-1] &= 0x7fffff; break;
            case 2:
                iq[jz-1] &= 0x3fffff; break;
            }
        }
        if (ih == 2) {
            z = 1.0 - z;
            if (carry != 0)
                z -= std::scalbn(1.0,q0);
        }
    }

    if (z == 0.0) {
        j = 0;
        for (i=jz-1; i>=jk; i--) j |= iq[i];
        if (j == 0) {
            for (k=1; iq[jk-k]==0; k++);

            for (i=jz+1; i<=jz+k; i++) {
                f[jx+i] = (double)ipio2[jv+i];
                for (j=0,fw=0.0; j<=jx; j++)
                    fw += x[j]*f[jx+i-j];
                q[i] = fw;
            }
            jz += k;
            goto recompute;
        }
    }

    if (z == 0.0) {
        jz -= 1;
        q0 -= 24;
        while (iq[jz] == 0) {
            jz--;
            q0 -= 24;
        }
    } else {
        z = std::scalbn(z,-q0);
        if (z >= 0x1p24) {
            fw = (double)(int32_t)(0x1p-24*z);
            iq[jz] = (int32_t)(z - 0x1p24*fw);
            jz += 1;
            q0 += 24;
            iq[jz] = (int32_t)fw;
        } else
            iq[jz] = (int32_t)z;
    }

    fw = std::scalbn(1.0,q0);
    for (i=jz; i>=0; i--) {
        q[i] = fw*(double)iq[i];
        fw *= 0x1p-24;
    }

    for(i=jz; i>=0; i--) {
        for (fw=0.0,k=0; k<=jp && k<=jz-i; k++)
            fw += PIo2[k]*q[i+k];
        fq[jz-i] = fw;
    }

    switch(prec) {
    case 0:
        fw = 0.0;
        for (i=jz; i>=0; i--)
            fw += fq[i];
        y[0] = ih==0 ? fw : -fw;
        break;
    case 1:
    case 2:
        fw = 0.0;
        for (i=jz; i>=0; i--)
            fw += fq[i];
        fw = (double)fw;
        y[0] = ih==0 ? fw : -fw;
        fw = fq[0]-fw;
        for (i=1; i<=jz; i++)
            fw += fq[i];
        y[1] = ih==0 ? fw : -fw;
        break;
    case 3:
        for (i=jz; i>0; i--) {
            fw      = fq[i-1]+fq[i];
            fq[i]  += fq[i-1]-fw;
            fq[i-1] = fw;
        }
        for (i=jz; i>1; i--) {
            fw      = fq[i-1]+fq[i];
            fq[i]  += fq[i-1]-fw;
            fq[i-1] = fw;
        }
        for (fw=0.0,i=jz; i>=2; i--)
            fw += fq[i];
        if (ih==0) {
            y[0] =  fq[0]; y[1] =  fq[1]; y[2] =  fw;
        } else {
            y[0] = -fq[0]; y[1] = -fq[1]; y[2] = -fw;
        }
    }
    return n&7;
}

// ---- __rem_pio2f (origin: FreeBSD e_rem_pio2f.c, via musl) ----
inline int remPio2f(float x, double *y)
{
    static const double
    toint   = 1.5/DBL_EPSILON,
    pio4    = 0x1.921fb6p-1,
    invpio2 = 6.36619772367581382433e-01,
    pio2_1  = 1.57079631090164184570e+00,
    pio2_1t = 1.58932547735281966916e-08;

    union { float f; uint32_t i; } u = {x};
    double tx[1],ty[1];
    double fn;
    uint32_t ix;
    int n, sign, e0;

    ix = u.i & 0x7fffffff;
    /* 25+53 bit pi is good enough for medium size */
    if (ix < 0x4dc90fdb) {  /* |x| ~< 2^28*(pi/2), medium size */
        /* Use a specialized rint() to get fn. */
        fn = (double)x*invpio2 + toint - toint;
        n  = (int32_t)fn;
        *y = x - fn*pio2_1 - fn*pio2_1t;
        /* Matters with directed rounding. */
        if (predictFalse(*y < -pio4)) {
            n--;
            fn--;
            *y = x - fn*pio2_1 - fn*pio2_1t;
        } else if (predictFalse(*y > pio4)) {
            n++;
            fn++;
            *y = x - fn*pio2_1 - fn*pio2_1t;
        }
        return n;
    }
    if(ix>=0x7f800000) {  /* x is inf or NaN */
        *y = x-x;
        return 0;
    }
    /* scale x into [2^23, 2^24-1] */
    sign = u.i>>31;
    e0 = (ix>>23) - (0x7f+23);  /* e0 = ilogb(|x|)-23, positive */
    u.i = ix - (e0<<23);
    tx[0] = u.f;
    n  =  remPio2Large(tx,ty,e0,1,0);
    if (sign) {
        *y = -ty[0];
        return -n;
    }
    *y = ty[0];
    return n;
}

} // namespace detail

// ---- tanhf (via musl) ----
inline float tanhf(float x)
{
    union { float f; uint32_t i; } u = {x};
    uint32_t w;
    int sign;
    float t;

    /* x = |x| */
    sign = u.i >> 31;
    u.i &= 0x7fffffff;
    x = u.f;
    w = u.i;

    if (w > 0x3f0c9f54) {
        /* |x| > log(3)/2 ~= 0.5493 or nan */
        if (w > 0x41200000) {
            /* |x| > 10 */
            t = 1 + 0/x;
        } else {
            t = detail::expm1f_impl(2*x);
            t = 1 - 2/(t+2);
        }
    } else if (w > 0x3e82c578) {
        /* |x| > log(5/3)/2 ~= 0.2554 */
        t = detail::expm1f_impl(2*x);
        t = t/(t+2);
    } else if (w >= 0x00800000) {
        /* |x| >= 0x1p-126 */
        t = detail::expm1f_impl(-2*x);
        t = -t/(t+2);
    } else {
        /* |x| is subnormal */
        detail::forceEval(x*x);
        t = x;
    }
    return sign ? -t : t;
}

// ---- tanhf over an array, without branches (same bits as tanhf above) ----
// tanhf branches on the size of its argument, and so does expm1f inside it.
// Across a creature's neurons those branches go any which way, and each wrong
// guess costs a pipeline flush. The functions below do, for every input,
// exactly the float operations tanhf and expm1f_impl would, but compute every
// case and SELECT the result rather than branch to it. That makes one call
// slower and an array faster, because the loop vectorises (SSE2 natively,
// SIMD128 in the browser module): 1.7x natively and 2.1x in wasm per value,
// while scalar code without SIMD would be slower than tanhf, so use these
// only on arrays. Exhaustively checked against tanhf on all 2^32 inputs
// (and on a sample by core_tests).
namespace detail {

inline float selectf(bool c, float a, float b) {   // c ? a : b, on the bits
    const uint32_t m = 0u - static_cast<uint32_t>(c);
    return bitsFloat((floatBits(a) & m) | (floatBits(b) & ~m));
}
inline int selecti(bool c, int a, int b) {
    const int m = -static_cast<int>(c);
    return (a & m) | (b & ~m);
}

// expm1f_impl for x in [-1.1, 20], the only range tanhfSelect passes it: no
// early returns are reachable there, k is -1..29, and each case's value is
// computed with expm1f_impl's own operations.
inline float expm1fSelect(float x)
{
    const float ln2_hi = 6.9313812256e-01f, ln2_lo = 9.0580006145e-06f,
                invln2 = 1.4426950216e+00f,
                Q1 = -3.3333212137e-2f, Q2 = 1.5807170421e-3f;
    const uint32_t ux = floatBits(x);
    const uint32_t hx = ux & 0x7fffffffu;
    const bool neg = (ux >> 31) != 0;

    const bool reduce = hx > 0x3eb17218u;      // |x| > 0.5 ln2
    const bool near1  = hx < 0x3F851592u;      // and |x| < 1.5 ln2: k = +-1
    const bool tiny   = hx < 0x33000000u;      // |x| < 2^-25: expm1(x) = x

    const int   kN  = static_cast<int>(invln2 * x + selectf(neg, -0.5f, 0.5f));
    const float tN  = static_cast<float>(kN);
    const float hi  = selectf(near1, selectf(neg, x + ln2_hi, x - ln2_hi), x - tN * ln2_hi);
    const float lo  = selectf(near1, selectf(neg, -ln2_lo, ln2_lo), tN * ln2_lo);
    const float xr  = hi - lo;
    const float cr  = (hi - xr) - lo;
    const float X   = selectf(reduce, xr, x);
    const float c   = selectf(reduce, cr, 0.0f);
    const int   k   = selecti(reduce, selecti(near1, selecti(neg, -1, 1), kN), 0);

    const float hfx = 0.5f * X;
    const float hxs = X * hfx;
    const float r1  = 1.0f + hxs * (Q1 + hxs * Q2);
    const float t   = 3.0f - r1 * hfx;
    const float e   = hxs * ((r1 - t) / (6.0f - X * t));
    const float k0  = X - (X * e - hxs);
    float e2 = X * (e - c) - c;
    e2 -= hxs;
    const float kM1 = 0.5f * (X - e2) - 0.5f;
    const float kP1 = selectf(X < -0.25f, -2.0f * (e2 - (X + 0.5f)), 1.0f + 2.0f * (X - e2));
    const float twopk = bitsFloat(static_cast<uint32_t>(0x7f + k) << 23);
    const float u     = bitsFloat(static_cast<uint32_t>(0x7f - k) << 23);
    const float kBig  = selectf(k < 23, (X - e2 + (1 - u)) * twopk, (X - (e2 + u) + 1) * twopk);

    float r = kBig;
    r = selectf(k == 1, kP1, r);
    r = selectf(k == -1, kM1, r);
    r = selectf(k == 0, k0, r);
    return selectf(!reduce && tiny, x, r);
}

inline float tanhfSelect(float x)
{
    const uint32_t ux = floatBits(x);
    const uint32_t sign = ux & 0x80000000u;
    const uint32_t w = ux & 0x7fffffffu;
    const float ax = bitsFloat(w);

    const bool mid = w > 0x3e82c578u;          // |x| > log(5/3)/2, and above
    float arg = selectf(mid, 2 * ax, -2 * ax);
    // |x| <= 10 keeps arg <= 20. Larger |x|, inf and NaN take the 1 + 0/x
    // case, so their expm1 is never used: clamp it into range (NaN -> 20).
    arg = selectf(arg <= 20.0f, arg, 20.0f);
    const float e = expm1fSelect(arg);
    const float tMid = e / (e + 2);

    float t = ax;                              // |x| subnormal or zero
    t = selectf(w >= 0x00800000u, -tMid, t);   // (-e)/(e+2): division is sign-symmetric
    t = selectf(mid, tMid, t);
    t = selectf(w > 0x3f0c9f54u, 1 - 2 / (e + 2), t);
    t = selectf(w > 0x41200000u, 1 + 0 / ax, t);
    return bitsFloat(floatBits(t) ^ sign);     // sign ? -t : t
}

} // namespace detail

// out[i] = tanhf(x[i]) for i < n, bit for bit; x and out must not overlap.
inline void tanhfArray(const float* __restrict x, float* __restrict out, int n)
{
    for (int i = 0; i < n; ++i) out[i] = detail::tanhfSelect(x[i]);
}

inline float expm1f(float x) { return detail::expm1f_impl(x); }

// ---- sinf (origin: FreeBSD s_sinf.c, via musl) ----
inline float sinf(float x)
{
    /* Small multiples of pi/2 rounded to double precision. */
    static const double
    s1pio2 = 1*detail::kPiOver2,
    s2pio2 = 2*detail::kPiOver2,
    s3pio2 = 3*detail::kPiOver2,
    s4pio2 = 4*detail::kPiOver2;

    double y;
    uint32_t ix;
    int n, sign;

    ix = detail::floatBits(x);
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if (ix <= 0x3f490fda) {  /* |x| ~<= pi/4 */
        if (ix < 0x39800000) {  /* |x| < 2**-12 */
            detail::forceEval(ix < 0x00800000 ? x/0x1p120f : x+0x1p120f);
            return x;
        }
        return detail::sindf(x);
    }
    if (ix <= 0x407b53d1) {  /* |x| ~<= 5*pi/4 */
        if (ix <= 0x4016cbe3) {  /* |x| ~<= 3pi/4 */
            if (sign)
                return -detail::cosdf(x + s1pio2);
            else
                return detail::cosdf(x - s1pio2);
        }
        return detail::sindf(sign ? -(x + s2pio2) : -(x - s2pio2));
    }
    if (ix <= 0x40e231d5) {  /* |x| ~<= 9*pi/4 */
        if (ix <= 0x40afeddf) {  /* |x| ~<= 7*pi/4 */
            if (sign)
                return detail::cosdf(x + s3pio2);
            else
                return -detail::cosdf(x - s3pio2);
        }
        return detail::sindf(sign ? x + s4pio2 : x - s4pio2);
    }

    /* sin(Inf or NaN) is NaN */
    if (ix >= 0x7f800000)
        return x - x;

    /* general argument reduction needed */
    n = detail::remPio2f(x, &y);
    switch (n&3) {
    case 0: return  detail::sindf(y);
    case 1: return  detail::cosdf(y);
    case 2: return  detail::sindf(-y);
    default:
        return -detail::cosdf(y);
    }
}

// ---- cosf (origin: FreeBSD s_cosf.c, via musl) ----
inline float cosf(float x)
{
    static const double
    c1pio2 = 1*detail::kPiOver2,
    c2pio2 = 2*detail::kPiOver2,
    c3pio2 = 3*detail::kPiOver2,
    c4pio2 = 4*detail::kPiOver2;

    double y;
    uint32_t ix;
    unsigned n, sign;

    ix = detail::floatBits(x);
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if (ix <= 0x3f490fda) {  /* |x| ~<= pi/4 */
        if (ix < 0x39800000) {  /* |x| < 2**-12 */
            detail::forceEval(x + 0x1p120f);
            return 1.0f;
        }
        return detail::cosdf(x);
    }
    if (ix <= 0x407b53d1) {  /* |x| ~<= 5*pi/4 */
        if (ix > 0x4016cbe3)  /* |x|  ~> 3*pi/4 */
            return -detail::cosdf(sign ? x+c2pio2 : x-c2pio2);
        else {
            if (sign)
                return detail::sindf(x + c1pio2);
            else
                return detail::sindf(c1pio2 - x);
        }
    }
    if (ix <= 0x40e231d5) {  /* |x| ~<= 9*pi/4 */
        if (ix > 0x40afeddf)  /* |x| ~> 7*pi/4 */
            return detail::cosdf(sign ? x+c4pio2 : x-c4pio2);
        else {
            if (sign)
                return detail::sindf(-x - c3pio2);
            else
                return detail::sindf(x - c3pio2);
        }
    }

    /* cos(Inf or NaN) is NaN */
    if (ix >= 0x7f800000)
        return x-x;

    /* general argument reduction needed */
    n = detail::remPio2f(x,&y);
    switch (n&3) {
    case 0: return  detail::cosdf(y);
    case 1: return  detail::sindf(-y);
    case 2: return -detail::cosdf(y);
    default:
        return  detail::sindf(y);
    }
}

// ---- atanf (origin: FreeBSD s_atanf.c, via musl) ----
inline float atanf(float x)
{
    static const float atanhi[] = {
      4.6364760399e-01f, /* atan(0.5)hi 0x3eed6338 */
      7.8539812565e-01f, /* atan(1.0)hi 0x3f490fda */
      9.8279368877e-01f, /* atan(1.5)hi 0x3f7b985e */
      1.5707962513e+00f, /* atan(inf)hi 0x3fc90fda */
    };
    static const float atanlo[] = {
      5.0121582440e-09f, /* atan(0.5)lo 0x31ac3769 */
      3.7748947079e-08f, /* atan(1.0)lo 0x33222168 */
      3.4473217170e-08f, /* atan(1.5)lo 0x33140fb4 */
      7.5497894159e-08f, /* atan(inf)lo 0x33a22168 */
    };
    static const float aT[] = {
      3.3333328366e-01f,
     -1.9999158382e-01f,
      1.4253635705e-01f,
     -1.0648017377e-01f,
      6.1687607318e-02f,
    };

    float w,s1,s2,z;
    uint32_t ix,sign;
    int id;

    ix = detail::floatBits(x);
    sign = ix>>31;
    ix &= 0x7fffffff;
    if (ix >= 0x4c800000) {  /* if |x| >= 2**26 */
        if (std::isnan(x))
            return x;
        z = atanhi[3] + 0x1p-120f;
        return sign ? -z : z;
    }
    if (ix < 0x3ee00000) {   /* |x| < 0.4375 */
        if (ix < 0x39800000) {  /* |x| < 2**-12 */
            if (ix < 0x00800000)
                detail::forceEval(x*x);
            return x;
        }
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3f980000) {  /* |x| < 1.1875 */
            if (ix < 0x3f300000) {  /*  7/16 <= |x| < 11/16 */
                id = 0;
                x = (2.0f*x - 1.0f)/(2.0f + x);
            } else {                /* 11/16 <= |x| < 19/16 */
                id = 1;
                x = (x - 1.0f)/(x + 1.0f);
            }
        } else {
            if (ix < 0x401c0000) {  /* |x| < 2.4375 */
                id = 2;
                x = (x - 1.5f)/(1.0f + 1.5f*x);
            } else {                /* 2.4375 <= |x| < 2**26 */
                id = 3;
                x = -1.0f/x;
            }
        }
    }
    /* end of argument reduction */
    z = x*x;
    w = z*z;
    /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
    s1 = z*(aT[0]+w*(aT[2]+w*aT[4]));
    s2 = w*(aT[1]+w*aT[3]);
    if (id < 0)
        return x - x*(s1+s2);
    z = atanhi[id] - ((x*(s1+s2) - atanlo[id]) - x);
    return sign ? -z : z;
}

// ---- atan2f (origin: FreeBSD e_atan2f.c, via musl) ----
inline float atan2f(float y, float x)
{
    static const float
    pi    = 3.1415927410e+00f,  /* 0x40490fdb */
    pi_lo = -8.7422776573e-08f; /* 0xb3bbbd2e */

    float z;
    uint32_t m,ix,iy;

    if (std::isnan(x) || std::isnan(y))
        return x+y;
    ix = detail::floatBits(x);
    iy = detail::floatBits(y);
    if (ix == 0x3f800000)  /* x=1.0 */
        return atanf(y);
    m = ((iy>>31)&1) | ((ix>>30)&2);  /* 2*sign(x)+sign(y) */
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;

    /* when y = 0 */
    if (iy == 0) {
        switch (m) {
        case 0:
        case 1: return y;   /* atan(+-0,+anything)=+-0 */
        case 2: return  pi; /* atan(+0,-anything) = pi */
        case 3: return -pi; /* atan(-0,-anything) =-pi */
        }
    }
    /* when x = 0 */
    if (ix == 0)
        return m&1 ? -pi/2 : pi/2;
    /* when x is INF */
    if (ix == 0x7f800000) {
        if (iy == 0x7f800000) {
            switch (m) {
            case 0: return  pi/4;   /* atan(+INF,+INF) */
            case 1: return -pi/4;   /* atan(-INF,+INF) */
            case 2: return 3*pi/4;  /* atan(+INF,-INF) */
            case 3: return -3*pi/4; /* atan(-INF,-INF) */
            }
        } else {
            switch (m) {
            case 0: return  0.0f;   /* atan(+...,+INF) */
            case 1: return -0.0f;   /* atan(-...,+INF) */
            case 2: return  pi;     /* atan(+...,-INF) */
            case 3: return -pi;     /* atan(-...,-INF) */
            }
        }
    }
    /* |y/x| > 0x1p26 */
    if (ix+(26<<23) < iy || iy == 0x7f800000)
        return m&1 ? -pi/2 : pi/2;

    /* z = atan(|y/x|) with correct underflow */
    if ((m&2) && iy+(26<<23) < ix)  /* |y/x| < 0x1p-26, x < 0 */
        z = 0.0f;
    else
        z = atanf(std::fabs(y/x));
    switch (m) {
    case 0: return z;              /* atan(+,+) */
    case 1: return -z;             /* atan(-,+) */
    case 2: return pi - (z-pi_lo); /* atan(+,-) */
    default: /* case 3 */
        return (z-pi_lo) - pi;     /* atan(-,-) */
    }
}

// ============================================================
// expf, logf (origin: Arm optimized-routines, via musl) and double tanh,
// expm1 (via musl; expm1 from FreeBSD s_expm1.c). Taken verbatim from the
// musl bundled with emsdk 6.0.4 (src/math/expf.c, exp2f_data.{c,h}, logf.c,
// logf_data.{c,h}, tanh.c, expm1.c and the helpers in src/internal/libm.h /
// src/math/__math_*.c), configured as musl builds them for wasm32:
// TOINT_INTRINSICS 0 (generic arch), WANT_ROUNDING 1, double_t == double.
// ============================================================
namespace detail {

inline uint64_t doubleBits(double x) { uint64_t u; std::memcpy(&u, &x, 8); return u; }
inline double bitsDouble(uint64_t u) { double x; std::memcpy(&x, &u, 8); return x; }

// musl's error-handling helpers (src/math/__math_xflowf.c, __math_oflowf.c,
// __math_uflowf.c, __math_divzerof.c, __math_invalidf.c). The values are
// computed exactly as there; what is dropped is errno (musl's math does not
// set it either) and fp_barrierf, which only keeps the compiler from folding
// the operation away so that the FP exception flag gets raised — the sim
// never reads FP flags.
inline float mathXflowf(uint32_t sign, float y) { return (sign ? -y : y) * y; }
inline float mathOflowf(uint32_t sign) { return mathXflowf(sign, 0x1p97f); }   // +-inf
inline float mathUflowf(uint32_t sign) { return mathXflowf(sign, 0x1p-95f); }  // +-0
inline float mathDivzerof(uint32_t sign) { return (sign ? -1.0f : 1.0f) / 0.0f; }
inline float mathInvalidf(float x) { return (x - x) / (x - x); }

// ---- exp2f_data (origin: Arm optimized-routines, via musl) ----
/*
 * Shared data between expf, exp2f and powf.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
namespace exp2f_data {
constexpr int EXP2F_TABLE_BITS = 5;
constexpr int EXP2F_POLY_ORDER = 3;
constexpr int N = 1 << EXP2F_TABLE_BITS;
struct Data {
    uint64_t tab[1 << EXP2F_TABLE_BITS];
    double shift_scaled;
    double poly[EXP2F_POLY_ORDER];
    double shift;
    double invln2_scaled;
    double poly_scaled[EXP2F_POLY_ORDER];
};
inline constexpr Data data = {
  /* tab[i] = uint(2^(i/N)) - (i << 52-BITS)
     used for computing 2^(k/N) for an int |k| < 150 N as
     double(tab[k%N] + (k << 52-BITS)) */
  .tab = {
0x3ff0000000000000, 0x3fefd9b0d3158574, 0x3fefb5586cf9890f, 0x3fef9301d0125b51,
0x3fef72b83c7d517b, 0x3fef54873168b9aa, 0x3fef387a6e756238, 0x3fef1e9df51fdee1,
0x3fef06fe0a31b715, 0x3feef1a7373aa9cb, 0x3feedea64c123422, 0x3feece086061892d,
0x3feebfdad5362a27, 0x3feeb42b569d4f82, 0x3feeab07dd485429, 0x3feea47eb03a5585,
0x3feea09e667f3bcd, 0x3fee9f75e8ec5f74, 0x3feea11473eb0187, 0x3feea589994cce13,
0x3feeace5422aa0db, 0x3feeb737b0cdc5e5, 0x3feec49182a3f090, 0x3feed503b23e255d,
0x3feee89f995ad3ad, 0x3feeff76f2fb5e47, 0x3fef199bdd85529c, 0x3fef3720dcef9069,
0x3fef5818dcfba487, 0x3fef7c97337b9b5f, 0x3fefa4afa2a490da, 0x3fefd0765b6e4540,
  },
  .shift_scaled = 0x1.8p+52 / N,
  .poly = {
  0x1.c6af84b912394p-5, 0x1.ebfce50fac4f3p-3, 0x1.62e42ff0c52d6p-1,
  },
  .shift = 0x1.8p+52,
  .invln2_scaled = 0x1.71547652b82fep+0 * N,
  .poly_scaled = {
  0x1.c6af84b912394p-5/N/N/N, 0x1.ebfce50fac4f3p-3/N/N, 0x1.62e42ff0c52d6p-1/N,
  },
};
} // namespace exp2f_data

// ---- logf_data (origin: Arm optimized-routines, via musl) ----
/*
 * Data definition for logf.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
namespace logf_data {
constexpr int LOGF_TABLE_BITS = 4;
constexpr int LOGF_POLY_ORDER = 4;
struct Data {
    struct {
        double invc, logc;
    } tab[1 << LOGF_TABLE_BITS];
    double ln2;
    double poly[LOGF_POLY_ORDER - 1]; /* First order coefficient is 1.  */
};
inline constexpr Data data = {
  .tab = {
  { 0x1.661ec79f8f3bep+0, -0x1.57bf7808caadep-2 },
  { 0x1.571ed4aaf883dp+0, -0x1.2bef0a7c06ddbp-2 },
  { 0x1.49539f0f010bp+0, -0x1.01eae7f513a67p-2 },
  { 0x1.3c995b0b80385p+0, -0x1.b31d8a68224e9p-3 },
  { 0x1.30d190c8864a5p+0, -0x1.6574f0ac07758p-3 },
  { 0x1.25e227b0b8eap+0, -0x1.1aa2bc79c81p-3 },
  { 0x1.1bb4a4a1a343fp+0, -0x1.a4e76ce8c0e5ep-4 },
  { 0x1.12358f08ae5bap+0, -0x1.1973c5a611cccp-4 },
  { 0x1.0953f419900a7p+0, -0x1.252f438e10c1ep-5 },
  { 0x1p+0, 0x0p+0 },
  { 0x1.e608cfd9a47acp-1, 0x1.aa5aa5df25984p-5 },
  { 0x1.ca4b31f026aap-1, 0x1.c5e53aa362eb4p-4 },
  { 0x1.b2036576afce6p-1, 0x1.526e57720db08p-3 },
  { 0x1.9c2d163a1aa2dp-1, 0x1.bc2860d22477p-3 },
  { 0x1.886e6037841edp-1, 0x1.1058bc8a07ee1p-2 },
  { 0x1.767dcf5534862p-1, 0x1.4043057b6ee09p-2 },
  },
  .ln2 = 0x1.62e42fefa39efp-1,
  .poly = {
  -0x1.00ea348b88334p-2, 0x1.5575b0be00b6ap-2, -0x1.ffffef20a4123p-2,
  }
};
} // namespace logf_data

// ---- expm1 (origin: FreeBSD s_expm1.c, via musl) ----
/*
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 */
/* expm1(x)
 * Returns exp(x)-1, the exponential of x minus 1.
 * (Method, special cases and accuracy: see the long comment in musl's
 *  src/math/expm1.c, identical to FreeBSD's.)
 */
inline double expm1_impl(double x)
{
    static const double
    o_threshold = 7.09782712893383973096e+02, /* 0x40862E42, 0xFEFA39EF */
    ln2_hi      = 6.93147180369123816490e-01, /* 0x3fe62e42, 0xfee00000 */
    ln2_lo      = 1.90821492927058770002e-10, /* 0x3dea39ef, 0x35793c76 */
    invln2      = 1.44269504088896338700e+00, /* 0x3ff71547, 0x652b82fe */
    /* Scaled Q's: Qn_here = 2**n * Qn_above, for R(2*z) where z = hxs = x*x/2: */
    Q1 = -3.33333333333331316428e-02, /* BFA11111 111110F4 */
    Q2 =  1.58730158725481460165e-03, /* 3F5A01A0 19FE5585 */
    Q3 = -7.93650757867487942473e-05, /* BF14CE19 9EAADBB7 */
    Q4 =  4.00821782732936239552e-06, /* 3ED0CFCA 86E65239 */
    Q5 = -2.01099218183624371326e-07; /* BE8AFDB7 6E09C32D */

    double y, hi, lo, c = 0, t, e, hxs, hfx, r1, twopk;
    union { double f; uint64_t i; } u = {x};
    uint32_t hx = u.i>>32 & 0x7fffffff;
    int k, sign = u.i>>63;

    /* filter out huge and non-finite argument */
    if (hx >= 0x4043687A) {  /* if |x|>=56*ln2 */
        if (std::isnan(x))
            return x;
        if (sign)
            return -1;
        if (x > o_threshold) {
            x *= 0x1p1023;
            return x;
        }
    }

    /* argument reduction */
    if (hx > 0x3fd62e42) {  /* if  |x| > 0.5 ln2 */
        if (hx < 0x3FF0A2B2) {  /* and |x| < 1.5 ln2 */
            if (!sign) {
                hi = x - ln2_hi;
                lo = ln2_lo;
                k =  1;
            } else {
                hi = x + ln2_hi;
                lo = -ln2_lo;
                k = -1;
            }
        } else {
            k  = invln2*x + (sign ? -0.5 : 0.5);
            t  = k;
            hi = x - t*ln2_hi;  /* t*ln2_hi is exact here */
            lo = t*ln2_lo;
        }
        x = hi-lo;
        c = (hi-x)-lo;
    } else if (hx < 0x3c900000) {  /* |x| < 2**-54, return x */
        if (hx < 0x00100000)
            forceEval((float)x);
        return x;
    } else
        k = 0;

    /* x is now in primary range */
    hfx = 0.5*x;
    hxs = x*hfx;
    r1 = 1.0+hxs*(Q1+hxs*(Q2+hxs*(Q3+hxs*(Q4+hxs*Q5))));
    t  = 3.0-r1*hfx;
    e  = hxs*((r1-t)/(6.0 - x*t));
    if (k == 0)   /* c is 0 */
        return x - (x*e-hxs);
    e  = x*(e-c) - c;
    e -= hxs;
    /* exp(x) ~ 2^k (x_reduced - e + 1) */
    if (k == -1)
        return 0.5*(x-e) - 0.5;
    if (k == 1) {
        if (x < -0.25)
            return -2.0*(e-(x+0.5));
        return 1.0+2.0*(x-e);
    }
    u.i = (uint64_t)(0x3ff + k)<<52;  /* 2^k */
    twopk = u.f;
    if (k < 0 || k > 56) {  /* suffice to return exp(x)-1 */
        y = x - e + 1.0;
        if (k == 1024)
            y = y*2.0*0x1p1023;
        else
            y = y*twopk;
        return y - 1.0;
    }
    u.i = (uint64_t)(0x3ff - k)<<52;  /* 2^-k */
    if (k < 20)
        y = (x-e+(1-u.f))*twopk;
    else
        y = (x-(e+u.f)+1)*twopk;
    return y;
}

} // namespace detail

// ---- expf (origin: Arm optimized-routines, via musl) ----
/*
 * Single-precision e^x function.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
/*
EXP2F_TABLE_BITS = 5
EXP2F_POLY_ORDER = 3

ULP error: 0.502 (nearest rounding.)
Relative error: 1.69 * 2^-34 in [-ln2/64, ln2/64] (before rounding.)
Wrong count: 170635 (all nearest rounding wrong results with fma.)
Non-nearest ULP error: 1 (rounded ULP error)
*/
inline float expf(float x)
{
    constexpr int EXP2F_TABLE_BITS = detail::exp2f_data::EXP2F_TABLE_BITS;
    constexpr int N = detail::exp2f_data::N;
    const double InvLn2N = detail::exp2f_data::data.invln2_scaled;
    const uint64_t* const T = detail::exp2f_data::data.tab;
    const double* const C = detail::exp2f_data::data.poly_scaled;
    const double SHIFT = detail::exp2f_data::data.shift;
    auto top12 = [](float v) -> uint32_t { return detail::floatBits(v) >> 20; };

    uint32_t abstop;
    uint64_t ki, t;
    double kd, xd, z, r, r2, y, s;

    xd = (double)x;
    abstop = top12(x) & 0x7ff;
    if (detail::predictFalse(abstop >= top12(88.0f))) {
        /* |x| >= 88 or x is nan.  */
        if (detail::floatBits(x) == detail::floatBits(-INFINITY))
            return 0.0f;
        if (abstop >= top12(INFINITY))
            return x + x;
        if (x > 0x1.62e42ep6f) /* x > log(0x1p128) ~= 88.72 */
            return detail::mathOflowf(0);
        if (x < -0x1.9fe368p6f) /* x < log(0x1p-150) ~= -103.97 */
            return detail::mathUflowf(0);
    }

    /* x*N/Ln2 = k + r with r in [-1/2, 1/2] and int k.  */
    z = InvLn2N * xd;

    /* Round and convert z to int, the result is in [-150*N, 128*N] and
       ideally ties-to-even rule is used, otherwise the magnitude of r
       can be bigger which gives larger approximation error.
       (musl's !TOINT_INTRINSICS path, the one its wasm32 build takes.)  */
    kd = z + SHIFT;   /* eval_as_double */
    ki = detail::doubleBits(kd);
    kd -= SHIFT;
    r = z - kd;

    /* exp(x) = 2^(k/N) * 2^(r/N) ~= s * (C0*r^3 + C1*r^2 + C2*r + 1) */
    t = T[ki % N];
    t += ki << (52 - EXP2F_TABLE_BITS);
    s = detail::bitsDouble(t);
    z = C[0] * r + C[1];
    r2 = r * r;
    y = C[2] * r + 1;
    y = z * r2 + y;
    y = y * s;
    return (float)y;  /* eval_as_float */
}

// ---- logf (origin: Arm optimized-routines, via musl) ----
/*
 * Single-precision log function.
 *
 * Copyright (c) 2017-2018, Arm Limited.
 * SPDX-License-Identifier: MIT
 */
/*
LOGF_TABLE_BITS = 4
LOGF_POLY_ORDER = 4

ULP error: 0.818 (nearest rounding.)
Relative error: 1.957 * 2^-26 (before rounding.)
*/
inline float logf(float x)
{
    constexpr int LOGF_TABLE_BITS = detail::logf_data::LOGF_TABLE_BITS;
    constexpr int N = 1 << LOGF_TABLE_BITS;
    constexpr uint32_t OFF = 0x3f330000;
    const auto& T = detail::logf_data::data.tab;
    const double* const A = detail::logf_data::data.poly;
    const double Ln2 = detail::logf_data::data.ln2;

    double z, r, r2, y, y0, invc, logc;
    uint32_t ix, iz, tmp;
    int k, i;

    ix = detail::floatBits(x);
    /* Fix sign of zero with downward rounding when x==1.  */
    if (/* WANT_ROUNDING (1 in musl) && */ detail::predictFalse(ix == 0x3f800000))
        return 0;
    if (detail::predictFalse(ix - 0x00800000 >= 0x7f800000 - 0x00800000)) {
        /* x < 0x1p-126 or inf or nan.  */
        if (ix * 2 == 0)
            return detail::mathDivzerof(1);
        if (ix == 0x7f800000) /* log(inf) == inf.  */
            return x;
        if ((ix & 0x80000000) || ix * 2 >= 0xff000000)
            return detail::mathInvalidf(x);
        /* x is subnormal, normalize it.  */
        ix = detail::floatBits(x * 0x1p23f);
        ix -= 23 << 23;
    }

    /* x = 2^k z; where z is in range [OFF,2*OFF] and exact.
       The range is split into N subintervals.
       The ith subinterval contains z and c is near its center.  */
    tmp = ix - OFF;
    i = (tmp >> (23 - LOGF_TABLE_BITS)) % N;
    k = (int32_t)tmp >> 23; /* arithmetic shift */
    iz = ix - (tmp & 0xff800000);
    invc = T[i].invc;
    logc = T[i].logc;
    z = (double)detail::bitsFloat(iz);

    /* log(x) = log1p(z/c-1) + log(c) + k*Ln2 */
    r = z * invc - 1;
    y0 = logc + (double)k * Ln2;

    /* Pipelined polynomial evaluation to approximate log1p(r).  */
    r2 = r * r;
    y = A[1] * r + A[2];
    y = A[0] * r2 + y;
    y = y * r2 + (y0 + r);
    return (float)y;  /* eval_as_float */
}

// ---- tanh, double (via musl) ----
/* tanh(x) = (exp(x) - exp(-x))/(exp(x) + exp(-x))
 *         = (exp(2*x) - 1)/(exp(2*x) - 1 + 2)
 *         = (1 - exp(-2*x))/(exp(-2*x) - 1 + 2)
 */
inline double tanh(double x)
{
    union {double f; uint64_t i;} u = {x};
    uint32_t w;
    int sign;
    double t;

    /* x = |x| */
    sign = u.i >> 63;
    u.i &= (uint64_t)-1/2;
    x = u.f;
    w = u.i >> 32;

    if (w > 0x3fe193ea) {
        /* |x| > log(3)/2 ~= 0.5493 or nan */
        if (w > 0x40340000) {
            /* |x| > 20 or nan */
            /* note: this branch avoids raising overflow */
            t = 1 - 0/x;
        } else {
            t = detail::expm1_impl(2*x);
            t = 1 - 2/(t+2);
        }
    } else if (w > 0x3fd058ae) {
        /* |x| > log(5/3)/2 ~= 0.2554 */
        t = detail::expm1_impl(2*x);
        t = t/(t+2);
    } else if (w >= 0x00100000) {
        /* |x| >= 0x1p-1022, up to 2ulp error in [0.1,0.2554] */
        t = detail::expm1_impl(-2*x);
        t = -t/(t+2);
    } else {
        /* |x| is subnormal */
        /* note: the branch above would not raise underflow in [0x1p-1023,0x1p-1022) */
        detail::forceEval((float)x);
        t = x;
    }
    return sign ? -t : t;
}

} // namespace evomath
