/* Runtime for code translated by a2c.py: the register file, memory access, and the arm64 operations that C does
 * not express directly. Build with -ffp-contract=off and FMA enabled: a*b+c must stay two operations, and fma()
 * must be one. */
#ifndef A2C_RT_H
#define A2C_RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>

/* A SIMD register. The vector-typed members come first so that common operations stay in an xmm register. */
typedef union {
    __m128 m; __m128i i; __m128d md;
    uint64_t q[2]; int64_t sq[2]; uint32_t w[4]; int32_t sw[4]; uint16_t h[8]; int16_t sh[8];
    uint8_t b[16]; int8_t sb[16]; float f[4]; double d[2];
} V;

typedef struct cpu {
    uint64_t x[31];
    uint64_t sp;
    V v[32];
    uint64_t tls;           /* tpidr_el0 */
    uint64_t fpcr, fpsr;
} cpu_t;

typedef struct { uint64_t addr; void (*fn)(cpu_t *); } a2c_func;
typedef struct { const char *name; void (*fn)(cpu_t *); } a2c_export;

/* The images are mapped at fixed addresses, so addresses in the translated code are constants. */
#define IMG1 0x6a0000000000ULL
#define IMG2 0x6b0000000000ULL
#define A2C_HOST 0x6c0000000000ULL      /* fake addresses of host functions, 16 bytes apart */

/* The files the code was translated from, in the program itself (gen_images.c, written with the translation). */
extern const uint8_t a2c_image1[], a2c_image1_end[], a2c_image2[], a2c_image2_end[];

#define VZERO vzero()

void a2c_call(cpu_t *c, uint64_t addr);
void a2c_trap(cpu_t *c, uint64_t addr, const char *why);
uint64_t a2c_mrs(cpu_t *c, const char *reg);
void a2c_msr(cpu_t *c, const char *reg, uint64_t v);

#define A2C_INLINE static inline __attribute__((always_inline))

A2C_INLINE uint8_t ld8(uint64_t a) { return *(const uint8_t *)a; }
A2C_INLINE uint16_t ld16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)a, 2); return v; }
A2C_INLINE uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)a, 4); return v; }
A2C_INLINE uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)a, 8); return v; }
A2C_INLINE void st8(uint64_t a, uint8_t v) { *(uint8_t *)a = v; }
A2C_INLINE void st16(uint64_t a, uint16_t v) { memcpy((void *)a, &v, 2); }
A2C_INLINE void st32(uint64_t a, uint32_t v) { memcpy((void *)a, &v, 4); }
A2C_INLINE void st64(uint64_t a, uint64_t v) { memcpy((void *)a, &v, 8); }

A2C_INLINE V vzero(void) { V t; t.i = _mm_setzero_si128(); return t; }
A2C_INLINE V mk_b(uint8_t x) { V t; t.i = _mm_cvtsi32_si128(x); return t; }
A2C_INLINE V mk_h(uint16_t x) { V t; t.i = _mm_cvtsi32_si128(x); return t; }
A2C_INLINE V mk_w(uint32_t x) { V t; t.i = _mm_cvtsi32_si128((int)x); return t; }
A2C_INLINE V mk_q(uint64_t x) { V t; t.i = _mm_cvtsi64_si128((long long)x); return t; }
A2C_INLINE V mk_f(float x) { V t; t.m = _mm_set_ss(x); return t; }
A2C_INLINE V mk_d(double x) { V t; t.md = _mm_set_sd(x); return t; }

/* ---- whole-register operations, as SSE: the lanes compute what the arm64 instruction computes */
#define A2C_LOW64(x) _mm_move_epi64(x)      /* keep the low 64 bits, clear the rest */
A2C_INLINE __m128i ldq(uint64_t a) { return _mm_loadu_si128((const __m128i *)a); }
A2C_INLINE void stq(uint64_t a, __m128i v) { _mm_storeu_si128((__m128i *)a, v); }

/* FPMax, FPMin, FPMaxNum, FPMinNum on four floats: maxps/minps give the second operand on equality or NaN */
A2C_INLINE __m128 fmax4(__m128 a, __m128 b) {
    __m128 r = _mm_blendv_ps(_mm_max_ps(a, b), _mm_and_ps(a, b), _mm_cmpeq_ps(a, b));
    return _mm_blendv_ps(r, _mm_add_ps(a, b), _mm_cmpunord_ps(a, b));
}
A2C_INLINE __m128 fmin4(__m128 a, __m128 b) {
    __m128 r = _mm_blendv_ps(_mm_min_ps(a, b), _mm_or_ps(a, b), _mm_cmpeq_ps(a, b));
    return _mm_blendv_ps(r, _mm_add_ps(a, b), _mm_cmpunord_ps(a, b));
}
A2C_INLINE __m128 fmaxnm4(__m128 a, __m128 b) {
    __m128 an = _mm_cmpunord_ps(a, a), bn = _mm_cmpunord_ps(b, b);
    __m128 r = fmax4(a, b);
    r = _mm_blendv_ps(r, b, _mm_andnot_ps(bn, an));
    return _mm_blendv_ps(r, a, _mm_andnot_ps(an, bn));
}
A2C_INLINE __m128 fminnm4(__m128 a, __m128 b) {
    __m128 an = _mm_cmpunord_ps(a, a), bn = _mm_cmpunord_ps(b, b);
    __m128 r = fmin4(a, b);
    r = _mm_blendv_ps(r, b, _mm_andnot_ps(bn, an));
    return _mm_blendv_ps(r, a, _mm_andnot_ps(an, bn));
}

float frecpe32(float x);
extern int32_t a2c_recip_tab[256];
/* frecpe on four floats: for normal numbers whose reciprocal is normal the result is the sign, the exponent
 * 253 - e and the table's eight bits; anything else goes through the general routine. */
A2C_INLINE __m128 frecpe4(__m128 x) {
    __m128i u = _mm_castps_si128(x);
    __m128i e = _mm_and_si128(_mm_srli_epi32(u, 23), _mm_set1_epi32(0xff));
    __m128i odd = _mm_or_si128(_mm_cmpeq_epi32(e, _mm_setzero_si128()), _mm_cmpgt_epi32(e, _mm_set1_epi32(252)));
    if (__builtin_expect(_mm_movemask_epi8(odd), 0)) {
        float t[4]; _mm_storeu_ps(t, x);
        for (int k = 0; k < 4; k++) t[k] = frecpe32(t[k]);
        return _mm_loadu_ps(t);
    }
    __m128i est = _mm_i32gather_epi32(a2c_recip_tab, _mm_and_si128(_mm_srli_epi32(u, 15), _mm_set1_epi32(0xff)), 4);
    __m128i r = _mm_or_si128(_mm_and_si128(u, _mm_set1_epi32((int)0x80000000u)),
                             _mm_or_si128(_mm_slli_epi32(_mm_sub_epi32(_mm_set1_epi32(253), e), 23), _mm_slli_epi32(est, 15)));
    return _mm_castsi128_ps(r);
}

A2C_INLINE uint32_t ror32(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
A2C_INLINE uint64_t ror64(uint64_t x, unsigned n) { n &= 63; return n ? (x >> n) | (x << (64 - n)) : x; }
A2C_INLINE uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
A2C_INLINE uint64_t clz64(uint64_t x) { return x ? (uint64_t)__builtin_clzll(x) : 64; }
A2C_INLINE uint32_t cls32(uint32_t x) { return clz32(x ^ (uint32_t)((int32_t)x >> 1)) - 1; }
A2C_INLINE uint64_t cls64(uint64_t x) { return clz64(x ^ (uint64_t)((int64_t)x >> 1)) - 1; }
A2C_INLINE uint32_t rev32(uint32_t x) { return __builtin_bswap32(x); }
A2C_INLINE uint64_t rev64(uint64_t x) { return __builtin_bswap64(x); }
A2C_INLINE uint32_t rev1632(uint32_t x) { return ((x & 0xff00ff00u) >> 8) | ((x & 0x00ff00ffu) << 8); }
A2C_INLINE uint64_t rev1664(uint64_t x) { return ((x & 0xff00ff00ff00ff00ULL) >> 8) | ((x & 0x00ff00ff00ff00ffULL) << 8); }
A2C_INLINE uint64_t rev3264(uint64_t x) { return ((uint64_t)__builtin_bswap32((uint32_t)(x >> 32)) << 32) | __builtin_bswap32((uint32_t)x); }
A2C_INLINE uint32_t rbit32(uint32_t x) { uint32_t r = 0; for (int i = 0; i < 32; i++) r |= ((x >> i) & 1u) << (31 - i); return r; }
A2C_INLINE uint64_t rbit64(uint64_t x) { uint64_t r = 0; for (int i = 0; i < 64; i++) r |= ((x >> i) & 1ull) << (63 - i); return r; }
A2C_INLINE uint64_t umulh(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
A2C_INLINE uint64_t smulh(uint64_t a, uint64_t b) { return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); }
A2C_INLINE uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
A2C_INLINE uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
A2C_INLINE uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if (a == 0x80000000u && b == 0xffffffffu) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
A2C_INLINE uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if (a == 0x8000000000000000ULL && b == ~0ULL) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}

/* arm64 FPMax / FPMin / FPMaxNum / FPMinNum */
A2C_INLINE float fmax32(float a, float b) { if (a != a) return a; if (b != b) return b; if (a == 0 && b == 0) return signbit(a) ? b : a; return a > b ? a : b; }
A2C_INLINE float fmin32(float a, float b) { if (a != a) return a; if (b != b) return b; if (a == 0 && b == 0) return signbit(a) ? a : b; return a < b ? a : b; }
A2C_INLINE double fmax64(double a, double b) { if (a != a) return a; if (b != b) return b; if (a == 0 && b == 0) return signbit(a) ? b : a; return a > b ? a : b; }
A2C_INLINE double fmin64(double a, double b) { if (a != a) return a; if (b != b) return b; if (a == 0 && b == 0) return signbit(a) ? a : b; return a < b ? a : b; }
A2C_INLINE float fmaxnm32(float a, float b) { if (a != a) return b; if (b != b) return a; return fmax32(a, b); }
A2C_INLINE float fminnm32(float a, float b) { if (a != a) return b; if (b != b) return a; return fmin32(a, b); }
A2C_INLINE double fmaxnm64(double a, double b) { if (a != a) return b; if (b != b) return a; return fmax64(a, b); }
A2C_INLINE double fminnm64(double a, double b) { if (a != a) return b; if (b != b) return a; return fmin64(a, b); }
#define fmaxnmv32 fmaxnm32
#define fminnmv32 fminnm32
#define fmaxv32 fmax32
#define fminv32 fmin32

A2C_INLINE float fabs32(float a) { return fabsf(a); }
A2C_INLINE double fabs64(double a) { return fabs(a); }
A2C_INLINE float sqrt32(float a) { return sqrtf(a); }
A2C_INLINE double sqrt64(double a) { return sqrt(a); }
A2C_INLINE float floor32(float a) { return floorf(a); }
A2C_INLINE double floor64(double a) { return floor(a); }
A2C_INLINE float ceil32(float a) { return ceilf(a); }
A2C_INLINE double ceil64(double a) { return ceil(a); }
A2C_INLINE float trunc32(float a) { return truncf(a); }
A2C_INLINE double trunc64(double a) { return trunc(a); }
A2C_INLINE float round32(float a) { return roundf(a); }
A2C_INLINE double round64(double a) { return round(a); }
A2C_INLINE float roundeven32(float a) { return nearbyintf(a); }     /* the rounding mode is never changed */
A2C_INLINE double roundeven64(double a) { return nearbyint(a); }

/* float to integer, saturating, NaN to 0, as the fcvt* instructions do. The argument is already rounded. */
A2C_INLINE uint32_t f2s32(double x) { if (x != x) return 0; if (x >= 2147483648.0) return 0x7fffffffu; if (x <= -2147483648.0) return 0x80000000u; return (uint32_t)(int32_t)x; }
A2C_INLINE uint32_t f2u32(double x) { if (x != x || x <= 0) return 0; if (x >= 4294967296.0) return 0xffffffffu; return (uint32_t)x; }
A2C_INLINE uint64_t f2s64(double x) { if (x != x) return 0; if (x >= 9223372036854775808.0) return 0x7fffffffffffffffULL; if (x <= -9223372036854775808.0) return 0x8000000000000000ULL; return (uint64_t)(int64_t)x; }
A2C_INLINE uint64_t f2u64(double x) { if (x != x || x <= 0) return 0; if (x >= 18446744073709551616.0) return ~0ULL; return (uint64_t)x; }

double frecpe64(double x);
float frsqrte32(float x);
double frsqrte64(double x);
float half_to_f32(uint16_t h);
uint16_t f32_to_half(float f);
uint16_t f64_to_half(double f);

A2C_INLINE uint64_t ushl64(uint64_t v, int8_t s) { return s >= 64 || s <= -64 ? 0 : s >= 0 ? v << s : v >> -s; }
A2C_INLINE uint32_t ushl32(uint32_t v, int8_t s) { return s >= 32 || s <= -32 ? 0 : s >= 0 ? v << s : v >> -s; }
A2C_INLINE uint16_t ushl16(uint16_t v, int8_t s) { return s >= 16 || s <= -16 ? 0 : (uint16_t)(s >= 0 ? v << s : v >> -s); }
A2C_INLINE uint8_t ushl8(uint8_t v, int8_t s) { return s >= 8 || s <= -8 ? 0 : (uint8_t)(s >= 0 ? v << s : v >> -s); }
A2C_INLINE int64_t sshl64(int64_t v, int8_t s) { return s >= 64 ? 0 : s >= 0 ? (int64_t)((uint64_t)v << s) : v >> (s <= -64 ? 63 : -s); }
A2C_INLINE int32_t sshl32(int32_t v, int8_t s) { return s >= 32 ? 0 : s >= 0 ? (int32_t)((uint32_t)v << s) : v >> (s <= -32 ? 31 : -s); }
A2C_INLINE int16_t sshl16(int16_t v, int8_t s) { return s >= 16 ? 0 : (int16_t)(s >= 0 ? (uint16_t)v << s : v >> (s <= -16 ? 15 : -s)); }
A2C_INLINE int8_t sshl8(int8_t v, int8_t s) { return s >= 8 ? 0 : (int8_t)(s >= 0 ? (uint8_t)v << s : v >> (s <= -8 ? 7 : -s)); }

#endif
