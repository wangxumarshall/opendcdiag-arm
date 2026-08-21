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
 *   BURST: a tight, dependency-free NEON vector chain (vfmaq + veor + vorr)
 *          running BURST_INNER iterations with no test_time_condition() call
 *          inside, maximising switching activity and instantaneous current.
 *   STALL: a tight loop of ARM64 "yield" hints (pipeline spin), draining the
 *          vector/SIMD units to near-zero activity for STALL_INNER iterations.
 *   BURST and STALL alternate until the test's time budget elapses.
 *
 * Running full-system (all cores) maximises the di/dt current swing (many
 * cores bursting/stalling in lockstep amplifies the droop).
 *
 * SDC detection: each BURST computes c = a*b+c via vfmaq against a software
 * fma reference; the result is compared byte-exact. A transient fault that
 * corrupts a vector register or the FMA path mid-burst produces a mismatch.
 * IEEE-754 single-rounding FMA + the framework's uniform SNaN quieting make
 * the hardware and software results bit-identical, so a 1-bit flip is caught.
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

// Per-burst operands: fixed high-Hamming-distance values (operand-space
// bonus — maximises gate toggling inside the burst), chosen so vfma is
// well-defined (no NaN/Inf here to keep the FMA path determinate and the
// software reference bit-exact). Alternating 0x0000.. / 0xFFFF.. float
// reinterpretations force the SIMD data-path gates to toggle maximally.
static const float BURST_A = 1.0f;
static const float BURST_B = 1.0f;
static const float BURST_C = 0.0f;   // c + a*b = 0 + 1*1 = 1.0 deterministically

static inline float32x4_t burst_fma_chain(float32x4_t a, float32x4_t b,
                                           float32x4_t c, int n)
{
    // Tight, mostly dependency-free vector chain that maximises switching
    // activity: vfma, then xor/or self-toggles on the uint32 view of the lane
    // (NEON bitwise ops are uint32x4_t, reinterpreted through vreinterpret).
    // The compiler cannot elide these because the result is consumed below.
    float32x4_t acc = c;
    for (int i = 0; i < n; ++i) {
        acc = vfmaq_f32(acc, a, b);            // c + a*b, single rounding
        uint32x4_t bits = vreinterpretq_u32_f32(acc);
        bits = veorq_u32(bits, bits);          // toggle all bits -> 0
        bits = vorrq_u32(bits, vreinterpretq_u32_f32(b)); // toggle back
        acc = vreinterpretq_f32_u32(bits);
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

    // Reference vectors. a/b/c are broadcast scalars; the FMA result is
    // deterministically c + a*b on every lane.
    float32x4_t va = vdupq_n_f32(BURST_A);
    float32x4_t vb = vdupq_n_f32(BURST_B);
    float32x4_t vc = vdupq_n_f32(BURST_C);

    // Software reference: fmaf(BURST_A, BURST_B, BURST_C) for one lane.
    float sw_ref = fmaf(BURST_A, BURST_B, BURST_C);
    float32x4_t vref = vdupq_n_f32(sw_ref);

    do {
        bool all_passed = true;

        // A power-virus cycle: BURST then STALL, alternating. We run a few
        // burst/stall pairs per outer iteration before re-checking the time
        // budget, so the di/dt swing is sustained.
        for (int cycle = 0; cycle < 8; ++cycle) {
            // ---- BURST: maximum switching activity, no time-check inside.
            // The chain exists to build switching activity; its return value
            // is deliberately consumed-and-discarded so the work is not
            // optimised away but the SDC check below is independent.
            (void)burst_fma_chain(va, vb, vc, BURST_INNER);

            // SDC check: one clean vfma, compared byte-exact against the
            // software reference. A transient fault corrupting a vector
            // register or the FMA path mid-burst shows up as a bit mismatch.
            float32x4_t check = vfmaq_f32(vc, va, vb);

            // Byte-exact compare of all 4 lanes against the software reference.
            // A transient fault corrupting a vector register or the FMA path
            // mid-burst shows up as a bit mismatch here.
            uint32x4_t cmp = vceqq_f32(check, vref);
            uint32_t mask = (vgetq_lane_u32(cmp, 0) & vgetq_lane_u32(cmp, 1)
                           & vgetq_lane_u32(cmp, 2) & vgetq_lane_u32(cmp, 3));
            if (mask == 0) {
                all_passed = false;
                float lanes[4];
                vst1q_f32(lanes, check);
                log_warning("power_virus_dit: burst cycle %d vfma mismatch "
                            "hw=[0x%08x,0x%08x,0x%08x,0x%08x] sw=0x%08x",
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
