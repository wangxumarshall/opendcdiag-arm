/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b power_virus_dit
 * @parblock
 * di/dt voltage-transient power-virus SDC test (research dimension "版图三
 * 瞬态故障动态激发"). The research explicitly calls out: "write code that
 * causes violent local power-consumption swings (a power virus) — e.g. run the
 * NEON (SIMD) unit at full load for 10 cycles, then fully stall for 10 cycles,
 * alternating. The violent current change (di/dt) causes local voltage droop
 * (Voltage Droop), at which point otherwise-correct timing collapses and a
 * transient SDC is very likely to be excited."
 *
 * Static GATE coverage only catches stuck-at faults; most SDC arises from
 * transient faults (voltage droop, thermal noise, crosstalk). No existing
 * test in the suite implements the burst/stall alternation pattern — every
 * other test runs a steady-state loop. This is the first di/dt power-virus.
 *
 * Structure of one cycle:
 *   BURST: a tight *dependent* NEON FMA chain — acc_{k} = vfmaq(acc_{k-1},
 *          a_k, b_k) — where the operands a_k/b_k *alternate between two
 *          high-Hamming-distance values every step* (e.g. 1.0f / -2.0f,
 *          bit patterns 0x3f800000 / 0xc0000000). The dependency keeps the
 *          FMA pipe back-to-back (max instantaneous current), and the
 *          alternating operands force the multiplier input nets to toggle
 *          maximally every cycle (max switching activity / di/dt). The chain
 *          runs BURST_INNER iterations with no test_time_condition() inside.
 *   STALL: a tight loop of ARM64 "yield" hints (pipeline spin), draining the
 *          vector/SIMD units to near-zero activity for STALL_INNER iterations.
 *   BURST and STALL alternate until the test's time budget elapses.
 *
 * Running full-system (all cores) maximises the di/dt current swing (many
 * cores bursting/stalling in lockstep amplifies the droop).
 *
 * SDC detection: each BURST's *final accumulator* is compared byte-exact
 * against a software reference that repeats the identical dependent FMA
 * sequence with libm fmaf (single rounding per step, IEEE-754). Because the
 * chain is dependent and the operands are fixed, the final result is
 * deterministic and bit-identical between NEON and libm — so a 1-bit flip
 * anywhere in the burst (a transient fault corrupting a vector register or
 * the FMA path mid-burst) shows up as a mismatch in the accumulator. (The
 * previous version discarded the burst result and checked only a single
 * detached post-burst FMA, so a mid-burst fault was largely uncaught; and its
 * "toggle" was `veorq_u32(bits, bits)` which is identically 0 — a no-op that
 * delivered no switching activity at all.)
 *
 * ARM64-native (NEON vfma + yield); on non-aarch64 _run returns EXIT_SKIP.
 * Wired into the arm64 subdir (entered only under the aarch64 meson guard), so
 * x86-64 is untouched.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <limits>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

// Type-punning helper: copy a float's bit pattern into an integer without
// violating strict-aliasing (mirrors sandstone_data.cpp's memcpy approach;
// the framework treats -Wstrict-aliasing as an error under -Wextra).
static inline uint32_t bits_of(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

// Number of NEON operations in a burst before the stall. Chosen large enough
// to build real current / switching activity but small enough that a single
// burst is well under the framework's loop-check granularity.
static constexpr int BURST_INNER = 2048;
// Number of yield hints in a stall. Roughly matched to the burst length so
// the burst/stall duty cycle is ~50% (maximises di/dt swing).
static constexpr int STALL_INNER = 2048;

static int power_virus_dit_init(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

// Per-burst operand pairs. Two high-Hamming-distance float pairs alternate
// every step of the FMA chain: the multiplier input nets flip between very
// different bit patterns each cycle (max switching activity / di/dt), while
// the FMA stays a dependent back-to-back chain (acc_{k} = acc_{k-1} + a_k*b_k).
// All values are well-defined (no NaN/Inf) so the FMA path is determinate and
// the software fmaf reference is bit-exact. Bit patterns:
//   A0 = 1.0f  (0x3f800000),  A1 = -2.0f (0xc0000000)   — high Hamming
//   B0 = 4.0f  (0x40800000),  B1 = 0.125f(0x3e000000)   — high Hamming
static const float BURST_A0 = 1.0f;
static const float BURST_A1 = -2.0f;
static const float BURST_B0 = 4.0f;
static const float BURST_B1 = 0.125f;
static const float BURST_C  = 0.0f;   // initial accumulator

// The dependent FMA burst. Returns the final accumulator; the caller compares
// it byte-exact against a software reference that repeats the identical
// sequence with fmaf. A 1-bit fault anywhere in the burst corrupts the
// returned accumulator.
static inline float32x4_t burst_fma_chain(int n)
{
    float32x4_t a0 = vdupq_n_f32(BURST_A0);
    float32x4_t a1 = vdupq_n_f32(BURST_A1);
    float32x4_t b0 = vdupq_n_f32(BURST_B0);
    float32x4_t b1 = vdupq_n_f32(BURST_B1);
    float32x4_t acc = vdupq_n_f32(BURST_C);
    // Unrolled by 2 so each loop body does one (a0,b0) step then one (a1,b1)
    // step — the alternating operands that maximise multiplier-input toggling.
    int n2 = n / 2;
    for (int i = 0; i < n2; ++i) {
        acc = vfmaq_f32(acc, a0, b0);   // acc += A0*B0
        acc = vfmaq_f32(acc, a1, b1);   // acc += A1*B1  (operands toggle)
    }
    return acc;
}

// Software reference for the burst: identical dependent fmaf sequence.
static inline float burst_fma_reference(int n)
{
    float acc = BURST_C;
    int n2 = n / 2;
    for (int i = 0; i < n2; ++i) {
        acc = fmaf(BURST_A0, BURST_B0, acc);
        acc = fmaf(BURST_A1, BURST_B1, acc);
    }
    return acc;
}

static inline void stall_yield(int n)
{
    // Drain the vector/SIMD units to near-zero activity. "yield" is the ARM
    // hint that signals a spin-wait; it lets the core throttle down, which is
    // exactly the low-activity half of the di/dt cycle.
    for (int i = 0; i < n; ++i) {
        __asm__ volatile("yield" ::: "memory");
    }
}

static int power_virus_dit_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    // Software reference for the burst's final accumulator (one lane).
    float sw_ref = burst_fma_reference(BURST_INNER);
    float32x4_t vref = vdupq_n_f32(sw_ref);

    do {
        bool all_passed = true;

        // A power-virus cycle: BURST then STALL, alternating. We run a few
        // burst/stall pairs per outer iteration before re-checking the time
        // budget, so the di/dt swing is sustained.
        for (int cycle = 0; cycle < 8; ++cycle) {
            // ---- BURST: dependent FMA chain, alternating high-Hamming
            // operands, no time-check inside. Its final accumulator IS the SDC
            // check target — a transient fault anywhere in the burst corrupts
            // the returned value.
            float32x4_t acc = burst_fma_chain(BURST_INNER);

            // Byte-exact compare of all 4 lanes against the software reference.
            // A 1-bit flip in any lane of the burst accumulator is caught.
            uint32x4_t cmp = vceqq_f32(acc, vref);
            uint32_t mask = (vgetq_lane_u32(cmp, 0) & vgetq_lane_u32(cmp, 1)
                           & vgetq_lane_u32(cmp, 2) & vgetq_lane_u32(cmp, 3));
            if (mask == 0) {
                all_passed = false;
                float lanes[4];
                vst1q_f32(lanes, acc);
                log_warning("power_virus_dit: burst cycle %d accumulator "
                            "mismatch hw=[0x%08x,0x%08x,0x%08x,0x%08x] sw=0x%08x",
                            cycle,
                            bits_of(lanes[0]), bits_of(lanes[1]),
                            bits_of(lanes[2]), bits_of(lanes[3]),
                            bits_of(sw_ref));
                break;
            }

            // ---- STALL: near-zero activity, drain SIMD units.
            stall_yield(STALL_INNER);
        }

        if (!all_passed) {
            report_fail_msg("power_virus_dit: di/dt transient SDC detected "
                            "(NEON FMA corrupted mid burst/stall cycle)");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

#else

static int power_virus_dit_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "power-virus di/dt stress");
    return EXIT_SKIP;
}

#endif // __aarch64__

static int power_virus_dit_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(power_virus_dit,
             "di/dt voltage-transient power virus: alternating NEON full-load "
             "bursts with yield stalls to induce voltage droop and excite "
             "transient SDC on the ARM64 SIMD/FMA path")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = power_virus_dit_init,
    .test_run = power_virus_dit_run,
    .test_cleanup = power_virus_dit_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
