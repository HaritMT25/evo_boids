#pragma once
#include <cstdint>
#include <cstring>
#include <cfloat>
#include <cmath>

// ============================================================
// Portable float transcendentals for cross-toolchain bit-exactness.
//
// The sim's dynamics call tanh/sin/cos/atan2 on floats. glibc (native
// builds) and musl (Emscripten/wasm builds) implement these with different
// algorithms, and none of them are correctly-rounded, so the two libms
// disagree by 1 ulp on some inputs — enough to diverge a chaotic system.
// Measured 2026-07-25 (glibc 2.39 vs emsdk 6.0.4): tanhf, sinf, cosf,
// atan2f and expm1f differ; expf, logf and sqrtf are bit-identical (and
// sqrt is IEEE-exact everywhere), so those stay on the standard library.
//
// The fix: both builds compile THIS source instead of calling libm. The
// implementations are musl's (fdlibm lineage, taken verbatim from the
// musl copy bundled with emsdk), which are pure IEEE float/double
// arithmetic — with -ffp-contract=off they evaluate identically on every
// IEEE-754 target. Switching native code from glibc to these shifted
// trajectories once at v0.18.0 (a numerical-lineage break, same framing
// as -ffp-contract=off in 0.16.0); see the research log.
//
// musl licence: MIT. Original notices preserved below.
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

} // namespace evomath
