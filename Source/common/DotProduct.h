#pragma once

// Double-precision dot product for the FIR inner loops (true-peak detectors,
// polyphase oversampling). The project builds with strict IEEE semantics
// (-fno-fast-math, /fp:precise), so compilers will not vectorise a serial
// reduction; this helper uses explicit SIMD with four independent
// accumulators (SSE2 on x86-64, NEON on arm64, scalar fallback elsewhere).
// Unaligned loads: callers pass arbitrary offsets into history buffers.

#if defined(__AVX__)
 #include <immintrin.h>
 #define MERIDIAN_DOT_AVX 1     // MERIDIAN_ENABLE_AVX2=ON (/arch:AVX2 or -mavx2)
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
 #include <emmintrin.h>
 #define MERIDIAN_DOT_SSE2 1
#elif defined(__ARM_NEON) && defined(__aarch64__)
 #include <arm_neon.h>
 #define MERIDIAN_DOT_NEON 1
#endif

namespace meridian
{
inline double dotProduct (const double* a, const double* b, int n) noexcept
{
    int i = 0;
    double sum = 0.0;

#if defined(MERIDIAN_DOT_AVX)
    __m256d acc0 = _mm256_setzero_pd(), acc1 = _mm256_setzero_pd(), acc2 = _mm256_setzero_pd(), acc3 = _mm256_setzero_pd();

    for (; i + 16 <= n; i += 16)
    {
        acc0 = _mm256_add_pd (acc0, _mm256_mul_pd (_mm256_loadu_pd (a + i),      _mm256_loadu_pd (b + i)));
        acc1 = _mm256_add_pd (acc1, _mm256_mul_pd (_mm256_loadu_pd (a + i + 4),  _mm256_loadu_pd (b + i + 4)));
        acc2 = _mm256_add_pd (acc2, _mm256_mul_pd (_mm256_loadu_pd (a + i + 8),  _mm256_loadu_pd (b + i + 8)));
        acc3 = _mm256_add_pd (acc3, _mm256_mul_pd (_mm256_loadu_pd (a + i + 12), _mm256_loadu_pd (b + i + 12)));
    }

    for (; i + 4 <= n; i += 4)
        acc0 = _mm256_add_pd (acc0, _mm256_mul_pd (_mm256_loadu_pd (a + i), _mm256_loadu_pd (b + i)));

    const __m256d s4 = _mm256_add_pd (_mm256_add_pd (acc0, acc1), _mm256_add_pd (acc2, acc3));
    const __m128d s = _mm_add_pd (_mm256_castpd256_pd128 (s4), _mm256_extractf128_pd (s4, 1));
    sum = _mm_cvtsd_f64 (s) + _mm_cvtsd_f64 (_mm_unpackhi_pd (s, s));
#elif defined(MERIDIAN_DOT_SSE2)
    __m128d acc0 = _mm_setzero_pd(), acc1 = _mm_setzero_pd(), acc2 = _mm_setzero_pd(), acc3 = _mm_setzero_pd();

    for (; i + 8 <= n; i += 8)
    {
        acc0 = _mm_add_pd (acc0, _mm_mul_pd (_mm_loadu_pd (a + i),     _mm_loadu_pd (b + i)));
        acc1 = _mm_add_pd (acc1, _mm_mul_pd (_mm_loadu_pd (a + i + 2), _mm_loadu_pd (b + i + 2)));
        acc2 = _mm_add_pd (acc2, _mm_mul_pd (_mm_loadu_pd (a + i + 4), _mm_loadu_pd (b + i + 4)));
        acc3 = _mm_add_pd (acc3, _mm_mul_pd (_mm_loadu_pd (a + i + 6), _mm_loadu_pd (b + i + 6)));
    }

    const __m128d s = _mm_add_pd (_mm_add_pd (acc0, acc1), _mm_add_pd (acc2, acc3));
    sum = _mm_cvtsd_f64 (s) + _mm_cvtsd_f64 (_mm_unpackhi_pd (s, s));
#elif defined(MERIDIAN_DOT_NEON)
    float64x2_t acc0 = vdupq_n_f64 (0.0), acc1 = vdupq_n_f64 (0.0), acc2 = vdupq_n_f64 (0.0), acc3 = vdupq_n_f64 (0.0);

    for (; i + 8 <= n; i += 8)
    {
        acc0 = vaddq_f64 (acc0, vmulq_f64 (vld1q_f64 (a + i),     vld1q_f64 (b + i)));
        acc1 = vaddq_f64 (acc1, vmulq_f64 (vld1q_f64 (a + i + 2), vld1q_f64 (b + i + 2)));
        acc2 = vaddq_f64 (acc2, vmulq_f64 (vld1q_f64 (a + i + 4), vld1q_f64 (b + i + 4)));
        acc3 = vaddq_f64 (acc3, vmulq_f64 (vld1q_f64 (a + i + 6), vld1q_f64 (b + i + 6)));
    }

    sum = vaddvq_f64 (vaddq_f64 (vaddq_f64 (acc0, acc1), vaddq_f64 (acc2, acc3)));
#else
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;

    for (; i + 4 <= n; i += 4)
    {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }

    sum = (s0 + s1) + (s2 + s3);
#endif

    for (; i < n; ++i)
        sum += a[i] * b[i];

    return sum;
}
/** Dot product with a linear-phase (symmetric, h[j] == h[n-1-j]) kernel:
    folds x[j] + x[n-1-j] first, so only ceil(n/2) multiplies are needed. */
inline double symmetricDotProduct (const double* h, const double* x, int n) noexcept
{
    const int half = n / 2;
    int j = 0;
    double sum = 0.0;

#if defined(MERIDIAN_DOT_AVX)
    __m256d acc0 = _mm256_setzero_pd(), acc1 = _mm256_setzero_pd();

    for (; j + 8 <= half; j += 8)
    {
        // reversed loads: {x[n-4-j] .. x[n-1-j]} permuted to descending order
        const __m256d r0 = _mm256_loadu_pd (x + n - 4 - j);
        const __m256d r1 = _mm256_loadu_pd (x + n - 8 - j);
        const __m256d v0 = _mm256_permute2f128_pd (_mm256_permute_pd (r0, 0x5), _mm256_permute_pd (r0, 0x5), 0x01);
        const __m256d v1 = _mm256_permute2f128_pd (_mm256_permute_pd (r1, 0x5), _mm256_permute_pd (r1, 0x5), 0x01);
        const __m256d f0 = _mm256_add_pd (_mm256_loadu_pd (x + j),     v0);
        const __m256d f1 = _mm256_add_pd (_mm256_loadu_pd (x + j + 4), v1);
        acc0 = _mm256_add_pd (acc0, _mm256_mul_pd (_mm256_loadu_pd (h + j),     f0));
        acc1 = _mm256_add_pd (acc1, _mm256_mul_pd (_mm256_loadu_pd (h + j + 4), f1));
    }

    const __m256d s4 = _mm256_add_pd (acc0, acc1);
    const __m128d s = _mm_add_pd (_mm256_castpd256_pd128 (s4), _mm256_extractf128_pd (s4, 1));
    sum = _mm_cvtsd_f64 (s) + _mm_cvtsd_f64 (_mm_unpackhi_pd (s, s));
#elif defined(MERIDIAN_DOT_SSE2)
    __m128d acc0 = _mm_setzero_pd(), acc1 = _mm_setzero_pd();

    for (; j + 4 <= half; j += 4)
    {
        const __m128d r0 = _mm_loadu_pd (x + n - 2 - j);   // {x[n-2-j], x[n-1-j]}
        const __m128d r1 = _mm_loadu_pd (x + n - 4 - j);
        const __m128d f0 = _mm_add_pd (_mm_loadu_pd (x + j),     _mm_shuffle_pd (r0, r0, 1));
        const __m128d f1 = _mm_add_pd (_mm_loadu_pd (x + j + 2), _mm_shuffle_pd (r1, r1, 1));
        acc0 = _mm_add_pd (acc0, _mm_mul_pd (_mm_loadu_pd (h + j),     f0));
        acc1 = _mm_add_pd (acc1, _mm_mul_pd (_mm_loadu_pd (h + j + 2), f1));
    }

    const __m128d s = _mm_add_pd (acc0, acc1);
    sum = _mm_cvtsd_f64 (s) + _mm_cvtsd_f64 (_mm_unpackhi_pd (s, s));
#elif defined(MERIDIAN_DOT_NEON)
    float64x2_t acc0 = vdupq_n_f64 (0.0), acc1 = vdupq_n_f64 (0.0);

    for (; j + 4 <= half; j += 4)
    {
        const float64x2_t r0 = vld1q_f64 (x + n - 2 - j);
        const float64x2_t r1 = vld1q_f64 (x + n - 4 - j);
        const float64x2_t f0 = vaddq_f64 (vld1q_f64 (x + j),     vextq_f64 (r0, r0, 1));
        const float64x2_t f1 = vaddq_f64 (vld1q_f64 (x + j + 2), vextq_f64 (r1, r1, 1));
        acc0 = vaddq_f64 (acc0, vmulq_f64 (vld1q_f64 (h + j),     f0));
        acc1 = vaddq_f64 (acc1, vmulq_f64 (vld1q_f64 (h + j + 2), f1));
    }

    sum = vaddvq_f64 (vaddq_f64 (acc0, acc1));
#endif

    for (; j < half; ++j)
        sum += h[j] * (x[j] + x[n - 1 - j]);

    if ((n & 1) != 0)
        sum += h[half] * x[half];

    return sum;
}
} // namespace meridian
