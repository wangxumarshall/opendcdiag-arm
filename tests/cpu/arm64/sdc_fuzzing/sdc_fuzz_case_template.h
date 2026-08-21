// sdc_fuzz_case_template.h — shared trigger / CRC / judge logic for the 100
// core-179 SDC reproducer tests.
//
// Each generated case file (case_NNN_*.cpp) defines the SDC_FUZZ_* parameters,
// includes this header (which expands to static init/run/cleanup helpers), then
// emits its own DECLARE_TEST(id, desc) inline so the short-id generator script
// (scripts/generate-short-ids.pl, regex `^DECLARE_TEST\s*\((\w+)`) can see the
// test id directly in the source file.
//
// The logic mirrors the end-to-end-verified libc-only MRU
// (mru_eigenmc.c + esdiag_phase2.cpp mode 1):
//   - build an SPD sparse matrix A once (init), via the SAME LCG RNG as the MRU
//   - analyzePattern(A) ONCE (mode 1) or redo per iter (mode 0) — both verified
//     to trigger; mode 1 = highest rate (numeric-only = rename-table longest hold)
//   - loop factorize(A) + solve(b) -> x; CRC32(x) via ARM __crc32cb; compare to
//     golden crc; mismatch -> report_fail(test)  (framework-standard SDC report)
//
// The corruption signature (verified identical across MRU + opendcdiag) is:
// fixed elem[0], multi-bit (10-32 bit) data-aliasing, silent (no PMU/EDAC).
// We CRC the whole x vector (corruption is always at elem[0], so any fail moves
// the CRC). On fail we also log elem[0] actual vs golden + xor popcount so the
// verifier can confirm it is the core-179 SDC signature (not a random SEU).
//
// Parameters per case (set via #define before including this header):
//   SDC_FUZZ_N           matrix dimension  (>= 256; report §10.3 threshold)
//   SDC_FUZZ_MSEED       matrix LCG seed   (changes sparsity/nnz structure)
//   SDC_FUZZ_MODE        0 = compute(both) per iter; 1 = numeric-only (reuse symbolic)
//   SDC_FUZZ_BSEED       RHS vector b LCG seed (corruption is in writeback, b-independent)
//   SDC_FUZZ_CLUSTER     cluster label for reporting (string literal)
//   SDC_FUZZ_ID          unique test id token (e.g. sdc_fuzz_000 — unquoted, for DECLARE_TEST)
//   SDC_FUZZ_DESC        one-line description (string literal)
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Intel Corporation.

#include <sandstone.h>

#include <Eigen/Sparse>
#include <Eigen/Dense>

#include <arm_acle.h>
#include <cstdint>
#include <cstring>
#include <cmath>

struct SdcFuzzData {
    int n;
    Eigen::SparseMatrix<double> A;
    Eigen::VectorXd b;
    Eigen::VectorXd golden;
    uint32_t golden_crc;
};

// SAME LCG RNG as the verified MRU (mru_eigenmc.c / esdiag_phase2.cpp).
// Seeded per-case so different cases build different (but deterministic) A.
static uint32_t sdc_fuzz_rng;

static double sdc_fuzz_frand(void) {
    sdc_fuzz_rng = sdc_fuzz_rng * 1103515245u + 12345u;
    return ((sdc_fuzz_rng >> 8) & 0xffffff) / (double)0x1000000;
}

// Build an SPD sparse matrix A (symmetric, diag-dominant), matching the MRU's
// build_matrix() structure. N and nnz vary per case. Eigen::Triplet::value()
// is const, so we build the triplet list in two passes: collect off-diagonal
// entries (and their per-column |offdiag| sum), then push the diagonal entries
// with the diag-dominant value.
static void sdc_fuzz_build_matrix(SdcFuzzData *d, int N, uint32_t mseed, uint32_t bseed) {
    d->n = N;
    sdc_fuzz_rng = mseed;
    std::vector<Eigen::Triplet<double>> trip;
    trip.reserve((size_t)N * 16);
    std::vector<double> diagsum(N, 0.0);   // sum of |off-diagonal| per column
    // pass 1: off-diagonal entries (symmetric), accumulate |offdiag| per column
    for (int j = 0; j < N; ++j) {
        for (int i = j + 1; i < N; ++i) {
            if (sdc_fuzz_frand() < 0.1) {
                double v = sdc_fuzz_frand() - 0.5;
                trip.push_back({i, j, v});
                trip.push_back({j, i, v});
                diagsum[j] += std::fabs(v);
                diagsum[i] += std::fabs(v);
            }
        }
    }
    // pass 2: diagonal entries (diag-dominant: A(j,j) = sum|offdiag| + 2, SPD)
    for (int j = 0; j < N; ++j) {
        trip.push_back({j, j, diagsum[j] + 2.0});
    }
    d->A = Eigen::SparseMatrix<double>(N, N);
    d->A.setFromTriplets(trip.begin(), trip.end());

    // RHS vector b, independently seeded (corruption is writeback, b-independent)
    sdc_fuzz_rng = bseed;
    d->b = Eigen::VectorXd(N);
    for (int i = 0; i < N; ++i) d->b[i] = sdc_fuzz_frand() - 0.5;
}

// CRC32 over the whole x vector (byte-wise, ARM __crc32cb) — same as MRU.
static uint32_t sdc_fuzz_crc_x(const Eigen::VectorXd &x, int N) {
    uint32_t c = 0xffffffffu;
    const uint8_t *p = (const uint8_t *)x.data();
    for (int i = 0; i < N * 8; ++i) c = __crc32cb(c, p[i]);
    return ~c;
}

// popcount of the xor of the first double's bytes — used to confirm multi-bit
// data-aliasing signature (core-179 SDC: 10-32 bits), distinguishing from a
// random single-bit SEU (1 bit).
static int sdc_fuzz_popcount_elem0(const Eigen::VectorXd &x, const Eigen::VectorXd &golden) {
    const uint8_t *a = (const uint8_t *)x.data();
    const uint8_t *g = (const uint8_t *)golden.data();
    int pc = 0;
    for (int i = 0; i < 8; ++i) pc += __builtin_popcount((unsigned)(a[i] ^ g[i]));
    return pc;
}

// ---- init / run / cleanup (static, one TU per case file) ----

static int sdc_fuzz_init(struct test *test) {
    auto *d = new SdcFuzzData;
    try {
        sdc_fuzz_build_matrix(d, SDC_FUZZ_N, SDC_FUZZ_MSEED, SDC_FUZZ_BSEED);
    } catch (...) {
        delete d;
        log_skip(TestResourceIssueSkipCategory, "Exception on Eigen matrix build, most probably OOM");
        return EXIT_SKIP;
    }
    // golden: full compute(A).solve(b) once (matches opendcdiag eigen_sparse init)
    Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> gs;
    try {
        d->golden = gs.compute(d->A).solve(d->b);
    } catch (...) {
        delete d;
        log_skip(TestResourceIssueSkipCategory, "Exception on Eigen code, most probably OOM");
        return EXIT_SKIP;
    }
    if (gs.info() != Eigen::Success) {
        delete d;
        report_fail(test);
        return EXIT_FAILURE;
    }
    d->golden_crc = sdc_fuzz_crc_x(d->golden, d->n);
    test->data = d;
    return EXIT_SUCCESS;
}

static int sdc_fuzz_run(struct test *test, int /*cpu*/) {
    auto *d = static_cast<SdcFuzzData *>(test->data);
    const int N = d->n;

    // MODE 1 (numeric-only): reuse one symbolic pattern — the verified
    // highest-trigger form (rename-table longest hold, report §10.2).
    // MODE 0 (compute-both): redo symbolic+numeric each iter — still triggers,
    // lower rate; used by the ROB cluster to probe post-symbolic-refresh leak.
#if SDC_FUZZ_MODE == 1
    Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> s;
    s.analyzePattern(d->A);
    do {
        Eigen::VectorXd x;
        try {
            s.factorize(d->A);
            x = s.solve(d->b);
        } catch (...) {
            report_fail_msg("Exception on Eigen code, most probably OOM");
        }
        if (s.info() != Eigen::Success) { report_fail(test); return EXIT_FAILURE; }
        uint32_t c = sdc_fuzz_crc_x(x, N);
        if (c != d->golden_crc) {
            int pc = sdc_fuzz_popcount_elem0(x, d->golden);
            report_fail_msg("core179 SDC: x crc mismatch (0x%08x vs golden 0x%08x) "
                             "elem[0]=%.6g (golden %.6g) popcount=%d bits [%s]",
                             c, d->golden_crc, x[0], d->golden[0], pc, SDC_FUZZ_CLUSTER);
        }
    } while (test_time_condition(test));
#else
    do {
        Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> s;
        Eigen::VectorXd x;
        try {
            x = s.compute(d->A).solve(d->b);
        } catch (...) {
            report_fail_msg("Exception on Eigen code, most probably OOM");
        }
        if (s.info() != Eigen::Success) { report_fail(test); return EXIT_FAILURE; }
        uint32_t c = sdc_fuzz_crc_x(x, N);
        if (c != d->golden_crc) {
            int pc = sdc_fuzz_popcount_elem0(x, d->golden);
            report_fail_msg("core179 SDC: x crc mismatch (0x%08x vs golden 0x%08x) "
                             "elem[0]=%.6g (golden %.6g) popcount=%d bits [%s]",
                             c, d->golden_crc, x[0], d->golden[0], pc, SDC_FUZZ_CLUSTER);
        }
    } while (test_time_condition(test));
#endif
    return EXIT_SUCCESS;
}

static int sdc_fuzz_cleanup(struct test *test) {
    delete static_cast<SdcFuzzData *>(test->data);
    return EXIT_SUCCESS;
}
