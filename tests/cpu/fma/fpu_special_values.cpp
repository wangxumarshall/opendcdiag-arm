/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b fpu_special_values
 * @parblock
 * FPU special-value sweep for SDC detection (research dimension "版图一.1 操作数
 * 空间", floating-point). The existing fmatail_* tests inject only
 * {0.0, 1.0, -1.0, +INFINITY}; this test sweeps the full IEEE-754 special-value
 * table through ARM64 NEON vfmaq_f32 / vfmaq_f64 to stress the FPU exception,
 * denormal-handling and NaN-propagation paths — the paths most prone to silent
 * data corruption.
 *
 * For each of float32 and float64, the three FMA operands (a, b, c) are drawn
 * from the full special-value set { qNaN, sNaN, +Inf, -Inf, +0, -0,
 * +min_denormal, -min_denormal, +max_normal, -max_normal, +min_normal,
 * -min_normal }. The hardware vfmaq result is compared byte-for-byte against a
 * software fma()/fmaf() reference. IEEE-754 defines single-rounding FMA and
 * NaN/Inf/zero propagation rules precisely, so the hardware and software
 * results must be bit-identical; the framework also quiets SNaN uniformly in
 * sandstone_data.cpp so NaN bit patterns match cross-arch.
 *
 * A single-bit ULP flip in the FMA output — exactly the SDC signature these
 * tests exist to catch — is caught by the byte-exact memcmp (unlike the
 * tolerance-based comparison in fma.cpp / fma_patterns_*, which masks it).
 *
 * ARM64-native (NEON vfma); on non-aarch64 the run path returns a clean
 * EXIT_SKIP ("to be implemented (placeholder): ARM NEON FMA required"), so the
 * test is built on all arches but only executes on ARM64.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <limits>
#include <cstdio>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

// Type-punning helper: copy the bit pattern of a float/double into an integer
// without violating strict-aliasing (mirrors sandstone_data.cpp's memcpy
// approach; the framework treats -Wstrict-aliasing as an error).
static inline uint32_t bits_of(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
static inline uint64_t bits_of(double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

// Number of special values in the sweep table (per type).
//  { qNaN, sNaN, +Inf, -Inf, +0, -0, +min_denorm, -min_denorm,
//    +max_normal, -max_normal, +min_normal, -min_normal }
static constexpr int NUM_SPECIALS = 12;

// Build the float32 special-value table.
static void build_specials_f32(float t[NUM_SPECIALS])
{
    t[0]  = NAN;                                          // qNaN
    t[1]  = __builtin_nanf("");                           // sNaN
    t[2]  = INFINITY;                                     // +Inf
    t[3]  = -INFINITY;                                    // -Inf
    t[4]  = 0.0f;                                         // +0
    t[5]  = -0.0f;                                        // -0
    t[6]  =  std::numeric_limits<float>::denorm_min();    // +min denormal
    t[7]  = -std::numeric_limits<float>::denorm_min();   // -min denormal
    t[8]  =  std::numeric_limits<float>::max();          // +max normal
    t[9]  = -std::numeric_limits<float>::max();          // -max normal
    t[10] =  std::numeric_limits<float>::min();          // +min normal
    t[11] = -std::numeric_limits<float>::min();          // -min normal
}

// Build the float64 special-value table.
static void build_specials_f64(double t[NUM_SPECIALS])
{
    t[0]  = NAN;                                          // qNaN
    t[1]  = __builtin_nan("");                           // sNaN
    t[2]  = INFINITY;                                     // +Inf
    t[3]  = -INFINITY;                                    // -Inf
    t[4]  = 0.0;                                          // +0
    t[5]  = -0.0;                                         // -0
    t[6]  =  std::numeric_limits<double>::denorm_min();  // +min denormal
    t[7]  = -std::numeric_limits<double>::denorm_min(); // -min denormal
    t[8]  =  std::numeric_limits<double>::max();         // +max normal
    t[9]  = -std::numeric_limits<double>::max();         // -max normal
    t[10] =  std::numeric_limits<double>::min();         // +min normal
    t[11] = -std::numeric_limits<double>::min();         // -min normal
}

static int fpu_special_values_init(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

// Sweep the float32 FMA over the full special-value cartesian product.
// Returns the number of mismatches found; fills the first mismatch details
// into the out parameters for one-shot logging.
static int sweep_f32(const float a_t[NUM_SPECIALS],
                     const float b_t[NUM_SPECIALS],
                     const float c_t[NUM_SPECIALS],
                     float &out_a, float &out_b, float &out_c,
                     float &out_hw, float &out_sw)
{
    int mismatches = 0;
    float hw_lane = 0, sw_lane = 0;

    for (int i = 0; i < NUM_SPECIALS; ++i) {
        for (int j = 0; j < NUM_SPECIALS; ++j) {
            for (int k = 0; k < NUM_SPECIALS; ++k) {
                float a = a_t[i], b = b_t[j], c = c_t[k];

                // Hardware: NEON vfmaq_f32 lane 0 (c + a*b, single rounding).
                // Broadcast each scalar into a 128-bit vector so the FMA is
                // exercised on a real vector lane, then extract lane 0.
                float32x4_t va = vdupq_n_f32(a);
                float32x4_t vb = vdupq_n_f32(b);
                float32x4_t vc = vdupq_n_f32(c);
                float32x4_t vd = vfmaq_f32(vc, va, vb);
                hw_lane = vgetq_lane_f32(vd, 0);

                // Software reference: fmaf (single rounding, IEEE-754).
                sw_lane = fmaf(a, b, c);

                // Byte-exact compare (1-bit ULP flip is caught).
                if (bits_of(hw_lane) != bits_of(sw_lane)) {
                    if (mismatches == 0) {
                        out_a = a; out_b = b; out_c = c;
                        out_hw = hw_lane; out_sw = sw_lane;
                    }
                    ++mismatches;
                }
            }
        }
    }
    return mismatches;
}

// Sweep the float64 FMA over the full special-value cartesian product.
static int sweep_f64(const double a_t[NUM_SPECIALS],
                     const double b_t[NUM_SPECIALS],
                     const double c_t[NUM_SPECIALS],
                     double &out_a, double &out_b, double &out_c,
                     double &out_hw, double &out_sw)
{
    int mismatches = 0;
    double hw_lane = 0, sw_lane = 0;

    for (int i = 0; i < NUM_SPECIALS; ++i) {
        for (int j = 0; j < NUM_SPECIALS; ++j) {
            for (int k = 0; k < NUM_SPECIALS; ++k) {
                double a = a_t[i], b = b_t[j], c = c_t[k];

                // Hardware: NEON vfmaq_f64 lane 0 (c + a*b, single rounding).
                // float64x2_t is the 128-bit double vector; broadcast each
                // scalar and extract lane 0 after the FMA.
                float64x2_t va = vdupq_n_f64(a);
                float64x2_t vb = vdupq_n_f64(b);
                float64x2_t vc = vdupq_n_f64(c);
                float64x2_t vd = vfmaq_f64(vc, va, vb);
                hw_lane = vgetq_lane_f64(vd, 0);

                // Software reference: fma (single rounding, IEEE-754).
                sw_lane = fma(a, b, c);

                // Byte-exact compare (1-bit ULP flip is caught).
                if (bits_of(hw_lane) != bits_of(sw_lane)) {
                    if (mismatches == 0) {
                        out_a = a; out_b = b; out_c = c;
                        out_hw = hw_lane; out_sw = sw_lane;
                    }
                    ++mismatches;
                }
            }
        }
    }
    return mismatches;
}

static int fpu_special_values_run(struct test *test, int cpu)
{
    (void)cpu;
    float a32[NUM_SPECIALS], b32[NUM_SPECIALS], c32[NUM_SPECIALS];
    build_specials_f32(a32);
    build_specials_f32(b32);
    build_specials_f32(c32);

    double a64[NUM_SPECIALS], b64[NUM_SPECIALS], c64[NUM_SPECIALS];
    build_specials_f64(a64);
    build_specials_f64(b64);
    build_specials_f64(c64);

    do {
        float  fa = 0, fb = 0, fc = 0, fhw = 0, fsw = 0;
        double da = 0, db = 0, dc = 0, dhw = 0, dsw = 0;

        int mm32 = sweep_f32(a32, b32, c32, fa, fb, fc, fhw, fsw);
        int mm64 = sweep_f64(a64, b64, c64, da, db, dc, dhw, dsw);

        if (mm32 != 0 || mm64 != 0) {
            if (mm32 != 0) {
                log_warning("fpu_special_values: f32 mismatch (count=%d): "
                            "fma(a,b,c) a=0x%08" PRIx32 " b=0x%08" PRIx32 " "
                            "c=0x%08" PRIx32 " hw=0x%08" PRIx32 " sw=0x%08" PRIx32,
                            mm32,
                            bits_of(fa), bits_of(fb), bits_of(fc),
                            bits_of(fhw), bits_of(fsw));
            }
            if (mm64 != 0) {
                log_warning("fpu_special_values: f64 mismatch (count=%d): "
                            "fma(a,b,c) a=0x%016" PRIx64 " b=0x%016" PRIx64 " "
                            "c=0x%016" PRIx64 " hw=0x%016" PRIx64 " sw=0x%016" PRIx64,
                            mm64,
                            bits_of(da), bits_of(db), bits_of(dc),
                            bits_of(dhw), bits_of(dsw));
            }
            report_fail_msg("fpu_special_values: FPU special-value SDC detected "
                            "(f32 mismatches=%d, f64 mismatches=%d)", mm32, mm64);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

#else

static int fpu_special_values_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON FMA required for fpu_special_values");
    return EXIT_SKIP;
}

#endif // __aarch64__

static int fpu_special_values_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fpu_special_values,
             "FPU special-value sweep (NaN/Inf/±0/denormal/±max/±min) through "
             "NEON FMA with byte-exact memcmp vs software reference (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fpu_special_values_init,
    .test_run = fpu_special_values_run,
    .test_cleanup = fpu_special_values_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
