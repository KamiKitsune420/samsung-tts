/* Hand-written host versions of the engine's hottest routines. Each computes exactly what the original computes,
 * operation for operation per output value, so the result is bit-identical; only the order in which independent
 * outputs are worked on changes. The translation of the original stays available as O_<name> and takes every
 * case not handled here. Checked by rendering a corpus with and without these (tools/compare_runs.py). */
#include "a2c_rt.h"

void O__Z4gemmPfPKfiiS1_i(cpu_t *c);

/* gemm(float *y, const float *W, int n_out, int n_in, const float *x, int mode)
 *
 * mode 8: outputs come in blocks of eight; a block's weights are n_in rows of eight floats, and each output is
 * y[o] = fma(x[j], W[j][o], y[o]) for j = 0 .. n_in-1 in that order. The original does one block at a time, so
 * every step waits for the fused multiply-add before it. Several blocks at a time keep the FMA units busy.
 * Outputs past the last whole block, and the other modes, go to the original. */
#ifdef GEMM_STATS       /* which shapes the time goes to: build with /DGEMM_STATS */
#include <stdio.h>
#include <stdlib.h>
static struct { int n_out, n_in, mode; long calls; } g_shapes[64]; static int g_nshapes;
static long long g_x_total, g_x_zero, g_x_denormal, g_x_small;
long long g_dn_calls, g_dn_slow, g_dn_acc_zero;
static double g_macs, g_ticks; static long long g_gemm_calls;
long long g_steps, g_acc_dn, g_w_dn;
static void gemm_report(void) {
    fprintf(stderr, "gemm total: %lld calls, %.1f M multiply-adds, %.2f G ticks in the block loops\n", g_gemm_calls, g_macs / 1e6, g_ticks / 1e9);
    fprintf(stderr, "gemm normal-x steps sampled: %lld; denormal accumulator after: %lld; denormal weight: %lld\n", g_steps, g_acc_dn, g_w_dn);
    fprintf(stderr, "gemm denormal-x steps: %lld, of which %lld had to be computed (%lld with a zero accumulator lane)\n", g_dn_calls, g_dn_slow, g_dn_acc_zero);
    fprintf(stderr, "gemm inputs: %lld values, %lld zero, %lld denormal, %lld below 1e-19\n", g_x_total, g_x_zero, g_x_denormal, g_x_small);
    for (int i = 0; i < g_nshapes; i++)
        fprintf(stderr, "gemm %5d x %5d mode %d: %8ld calls, %10.1f M multiply-adds\n", g_shapes[i].n_out, g_shapes[i].n_in,
                g_shapes[i].mode, g_shapes[i].calls, (double)g_shapes[i].calls * g_shapes[i].n_out * g_shapes[i].n_in / 1e6);
}
static void gemm_note(int n_out, int n_in, int mode) {
    for (int i = 0; i < g_nshapes; i++)
        if (g_shapes[i].n_out == n_out && g_shapes[i].n_in == n_in && g_shapes[i].mode == mode) { g_shapes[i].calls++; return; }
    if (!g_nshapes) atexit(gemm_report);
    if (g_nshapes < 64) { g_shapes[g_nshapes].n_out = n_out; g_shapes[g_nshapes].n_in = n_in; g_shapes[g_nshapes].mode = mode; g_shapes[g_nshapes++].calls = 1; }
}
#endif

/* acc = fma(x, w, acc) for a denormal x, without the microcode assist an x86 CPU takes for denormal operands
 * (about a hundred times the cost of the operation; arm64 has no such penalty, and some 7% of the inputs are
 * denormal). With |x| < 2^-126 and |w| < 2^40 the product is below 2^-86, which is less than half a unit in the
 * last place of any acc with |acc| >= 2^-50: the correctly rounded sum is acc itself. Only when some lane does not
 * meet those bounds is the operation actually carried out. */
static inline __m256 fma_denormal_x(__m256 xv, __m256 w, __m256 acc) {
    const __m256 absmask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff));
    __m256 big_acc = _mm256_cmp_ps(_mm256_and_ps(acc, absmask), _mm256_set1_ps(0x1p-50f), _CMP_GE_OQ);
    __m256 small_w = _mm256_cmp_ps(_mm256_and_ps(w, absmask), _mm256_set1_ps(0x1p40f), _CMP_LT_OQ);
#ifdef GEMM_STATS
    extern long long g_dn_calls, g_dn_slow, g_dn_acc_zero;
static double g_macs, g_ticks; static long long g_gemm_calls;
long long g_steps, g_acc_dn, g_w_dn;
    g_dn_calls++;
    if (_mm256_movemask_ps(_mm256_and_ps(big_acc, small_w)) != 0xff) {
        g_dn_slow++;
        if (_mm256_movemask_ps(_mm256_cmp_ps(acc, _mm256_setzero_ps(), _CMP_EQ_OQ))) g_dn_acc_zero++;
    }
#endif
    if (_mm256_movemask_ps(_mm256_and_ps(big_acc, small_w)) == 0xff) return acc;
    return _mm256_fmadd_ps(xv, w, acc);
}
static inline int is_denormal(float f) { uint32_t u; memcpy(&u, &f, 4); return ((u & 0x7fffffffu) - 1u) < 0x7fffffu; }

void H__Z4gemmPfPKfiiS1_i(cpu_t *c) {
    float *y = (float *)c->x[0];
    const float *W = (const float *)c->x[1];
    int n_out = (int)c->x[2], n_in = (int)c->x[3];
    const float *x = (const float *)c->x[4];
    int mode = (int)c->x[5];
#ifdef GEMM_STATS
    gemm_note(n_out, n_in, mode);
    g_macs += (double)n_out * n_in; g_gemm_calls++;
    unsigned long long t_begin = __rdtsc();
    struct tsc_guard { int x; } guard_; (void)guard_;
    for (int j = 0; j < n_in; j++) { uint32_t u; memcpy(&u, x + j, 4); u &= 0x7fffffffu; g_x_total++; if (u == 0) g_x_zero++; else if (u < 0x00800000u) g_x_denormal++; else if (u < 0x20000000u) g_x_small++; }
#endif
    if (mode != 8 || n_out < 8 || n_in < 1) { O__Z4gemmPfPKfiiS1_i(c); return; }

    int blocks = n_out / 8;
    size_t stride = (size_t)n_in * 8;       /* floats of weights per block */
    int b = 0;
    /* Four blocks at a time, not eight: a block's weights are often a multiple of 4 KB long, so the streams
     * share their cache-set index, and more of them than the cache has ways evict each other. */
    for (; b + 4 <= blocks; b += 4) {
        const float *w = W + (size_t)b * stride;
        float *o = y + 8 * b;
        __m256 a0 = _mm256_loadu_ps(o), a1 = _mm256_loadu_ps(o + 8), a2 = _mm256_loadu_ps(o + 16), a3 = _mm256_loadu_ps(o + 24);
        for (int j = 0; j < n_in; j++) {
            __m256 xv = _mm256_broadcast_ss(x + j);
            const float *r = w + 8 * (size_t)j;
            if (__builtin_expect(is_denormal(x[j]), 0)) {
                a0 = fma_denormal_x(xv, _mm256_loadu_ps(r), a0);
                a1 = fma_denormal_x(xv, _mm256_loadu_ps(r + stride), a1);
                a2 = fma_denormal_x(xv, _mm256_loadu_ps(r + 2 * stride), a2);
                a3 = fma_denormal_x(xv, _mm256_loadu_ps(r + 3 * stride), a3);
                continue;
            }
            a0 = _mm256_fmadd_ps(xv, _mm256_loadu_ps(r), a0);
            a1 = _mm256_fmadd_ps(xv, _mm256_loadu_ps(r + stride), a1);
            a2 = _mm256_fmadd_ps(xv, _mm256_loadu_ps(r + 2 * stride), a2);
            a3 = _mm256_fmadd_ps(xv, _mm256_loadu_ps(r + 3 * stride), a3);
#ifdef GEMM_STATS
            {
                extern long long g_steps, g_acc_dn, g_w_dn;
                const __m256 am = _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff)), mn = _mm256_set1_ps(0x1p-126f), z = _mm256_setzero_ps();
                __m256 t = _mm256_and_ps(a0, am), wv = _mm256_and_ps(_mm256_loadu_ps(r), am);
                g_steps++;
                if (_mm256_movemask_ps(_mm256_and_ps(_mm256_cmp_ps(t, mn, _CMP_LT_OQ), _mm256_cmp_ps(t, z, _CMP_NEQ_OQ)))) g_acc_dn++;
                if (_mm256_movemask_ps(_mm256_and_ps(_mm256_cmp_ps(wv, mn, _CMP_LT_OQ), _mm256_cmp_ps(wv, z, _CMP_NEQ_OQ)))) g_w_dn++;
            }
#endif
        }
        _mm256_storeu_ps(o, a0); _mm256_storeu_ps(o + 8, a1); _mm256_storeu_ps(o + 16, a2); _mm256_storeu_ps(o + 24, a3);
    }
    for (; b < blocks; b++) {
        const float *w = W + (size_t)b * stride;
        __m256 a0 = _mm256_loadu_ps(y + 8 * b);
        for (int j = 0; j < n_in; j++) {
            __m256 xv = _mm256_broadcast_ss(x + j), wv = _mm256_loadu_ps(w + 8 * (size_t)j);
            a0 = is_denormal(x[j]) ? fma_denormal_x(xv, wv, a0) : _mm256_fmadd_ps(xv, wv, a0);
        }
        _mm256_storeu_ps(y + 8 * b, a0);
    }
#ifdef GEMM_STATS
    g_ticks += (double)(__rdtsc() - t_begin);
#endif
    if (blocks * 8 < n_out) {       /* the last few outputs: the original's own code, entered as a call with fewer than eight */
        c->x[0] = (uint64_t)(y + blocks * 8);
        c->x[1] = (uint64_t)(W + (size_t)blocks * stride);
        c->x[2] = (uint64_t)(n_out - blocks * 8);
        c->x[3] = (uint64_t)n_in; c->x[4] = (uint64_t)x; c->x[5] = 8;
        O__Z4gemmPfPKfiiS1_i(c);
    }
}

/* ---- lpcnet::compute_activation(FP &out, FP &in, int n, int type), float32, sigmoid (1) and tanh (2)
 *
 * An FP is {void *data; long bytes_per_element; long count; bool view}. The original's 4-wide loop computes each
 * element on its own (rational approximations built on the reciprocal estimate), so doing eight at a time changes
 * nothing. Elements left over when n is not a multiple of four are computed by a different formula in the
 * original; those calls, the other activation types and the half-precision models go to the original. */
void O__ZN6lpcnet18compute_activationERNS_2FPES1_ii(cpu_t *c);

static inline __m256 frecpe8(__m256 x) {
    __m256i u = _mm256_castps_si256(x);
    __m256i e = _mm256_and_si256(_mm256_srli_epi32(u, 23), _mm256_set1_epi32(0xff));
    __m256i odd = _mm256_or_si256(_mm256_cmpeq_epi32(e, _mm256_setzero_si256()), _mm256_cmpgt_epi32(e, _mm256_set1_epi32(252)));
    if (__builtin_expect(_mm256_movemask_epi8(odd), 0)) {
        float t[8]; _mm256_storeu_ps(t, x);
        for (int k = 0; k < 8; k++) t[k] = frecpe32(t[k]);
        return _mm256_loadu_ps(t);
    }
    __m256i est = _mm256_i32gather_epi32(a2c_recip_tab, _mm256_and_si256(_mm256_srli_epi32(u, 15), _mm256_set1_epi32(0xff)), 4);
    __m256i r = _mm256_or_si256(_mm256_and_si256(u, _mm256_set1_epi32((int)0x80000000u)),
                                _mm256_or_si256(_mm256_slli_epi32(_mm256_sub_epi32(_mm256_set1_epi32(253), e), 23), _mm256_slli_epi32(est, 15)));
    return _mm256_castsi256_ps(r);
}
/* arm64 FPMax and FPMin, eight floats */
static inline __m256 fmax8(__m256 a, __m256 b) {
    __m256 r = _mm256_blendv_ps(_mm256_max_ps(a, b), _mm256_and_ps(a, b), _mm256_cmp_ps(a, b, _CMP_EQ_OQ));
    return _mm256_blendv_ps(r, _mm256_add_ps(a, b), _mm256_cmp_ps(a, b, _CMP_UNORD_Q));
}
static inline __m256 fmin8(__m256 a, __m256 b) {
    __m256 r = _mm256_blendv_ps(_mm256_min_ps(a, b), _mm256_or_ps(a, b), _mm256_cmp_ps(a, b, _CMP_EQ_OQ));
    return _mm256_blendv_ps(r, _mm256_add_ps(a, b), _mm256_cmp_ps(a, b, _CMP_UNORD_Q));
}
#define BITS8(u) _mm256_castsi256_ps(_mm256_set1_epi32((int)(u)))
#define BITS4(u) _mm_castsi128_ps(_mm_set1_epi32((int)(u)))

void H__ZN6lpcnet18compute_activationERNS_2FPES1_ii(cpu_t *c) {
    const uint64_t *out_fp = (const uint64_t *)c->x[0], *in_fp = (const uint64_t *)c->x[1];
    int n = (int)c->x[2], type = (int)c->x[3];
    if (out_fp[1] != 4 || (type != 1 && type != 2) || n < 4 || (n & 3)) {
        O__ZN6lpcnet18compute_activationERNS_2FPES1_ii(c);
        return;
    }
    const float *in = (const float *)in_fp[0];
    float *out = (float *)out_fp[0];
    int i = 0;
    if (type == 1) {        /* sigmoid */
        const __m256 k90 = BITS8(0x42b40000u), k125 = _mm256_set1_ps(12.5f), k36 = BITS8(0x3ce38e39u), k3 = BITS8(0x3eaaaaabu),
                     k49 = BITS8(0x3ee38e39u), k6 = BITS8(0x3e2aaaabu), half = BITS8(0x3f000000u), one = _mm256_set1_ps(1.0f),
                     tiny = BITS8(0x322bcc77u);
        for (; i + 8 <= n; i += 8) {
            __m256 x = _mm256_loadu_ps(in + i);
            __m256 r = frecpe8(_mm256_fmadd_ps(x, x, k90));
            __m256 y = _mm256_mul_ps(_mm256_fmadd_ps(r, k125, k36), x);
            __m256 r2 = frecpe8(_mm256_fmadd_ps(y, y, k3));
            __m256 s = _mm256_fmadd_ps(y, _mm256_fmadd_ps(r2, k49, k6), half);
            _mm256_storeu_ps(out + i, fmax8(fmin8(s, one), tiny));
        }
        for (; i < n; i += 4) {
            __m128 x = _mm_loadu_ps(in + i);
            __m128 r = frecpe4(_mm_fmadd_ps(x, x, BITS4(0x42b40000u)));
            __m128 y = _mm_mul_ps(_mm_fmadd_ps(r, _mm_set1_ps(12.5f), BITS4(0x3ce38e39u)), x);
            __m128 r2 = frecpe4(_mm_fmadd_ps(y, y, BITS4(0x3eaaaaabu)));
            __m128 s = _mm_fmadd_ps(y, _mm_fmadd_ps(r2, BITS4(0x3ee38e39u), BITS4(0x3e2aaaabu)), BITS4(0x3f000000u));
            _mm_storeu_ps(out + i, fmax4(fmin4(s, _mm_set1_ps(1.0f)), BITS4(0x322bcc77u)));
        }
    } else {                /* tanh */
        const __m256 k225 = BITS8(0x41b40000u), k625 = _mm256_set1_ps(6.25f), k18 = BITS8(0x3d638e39u), k3 = BITS8(0x3eaaaaabu),
                     k89 = BITS8(0x3f638e39u), one = _mm256_set1_ps(1.0f), mone = _mm256_set1_ps(-1.0f);
        for (; i + 8 <= n; i += 8) {
            __m256 x = _mm256_loadu_ps(in + i);
            __m256 r = frecpe8(_mm256_fmadd_ps(x, x, k225));
            __m256 y = _mm256_mul_ps(_mm256_fmadd_ps(r, k625, k18), x);
            __m256 r2 = frecpe8(_mm256_fmadd_ps(y, y, k3));
            __m256 s = _mm256_mul_ps(y, _mm256_fmadd_ps(r2, k89, k3));
            _mm256_storeu_ps(out + i, fmax8(fmin8(s, one), mone));
        }
        for (; i < n; i += 4) {
            __m128 x = _mm_loadu_ps(in + i);
            __m128 r = frecpe4(_mm_fmadd_ps(x, x, BITS4(0x41b40000u)));
            __m128 y = _mm_mul_ps(_mm_fmadd_ps(r, _mm_set1_ps(6.25f), BITS4(0x3d638e39u)), x);
            __m128 r2 = frecpe4(_mm_fmadd_ps(y, y, BITS4(0x3eaaaaabu)));
            __m128 s = _mm_mul_ps(y, _mm_fmadd_ps(r2, BITS4(0x3f638e39u), BITS4(0x3eaaaaabu)));
            _mm_storeu_ps(out + i, fmax4(fmin4(s, _mm_set1_ps(1.0f)), _mm_set1_ps(-1.0f)));
        }
    }
}
