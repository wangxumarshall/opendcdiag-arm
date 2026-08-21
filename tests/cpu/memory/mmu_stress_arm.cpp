/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mmu_stress_arm
 * @parblock
 * MMU / TLB targeted stress test for SDC detection (research dimension
 * "版图二 脆弱性驱动的靶向施压"). The research explicitly calls out that
 * simple load/store tests are insufficient for the MMU: one must "generate
 * large numbers of cross-page-boundary accesses, unaligned accesses, accesses
 * that frequently trigger page faults, and frequent TLB-invalidate (TLBI)
 * sequences to torture the MMU state machine" (the Page-Table Walker state
 * machine, in particular).
 *
 * User space cannot issue the privileged TLBI instruction, but it can achieve
 * the equivalent — forcing TLB misses + Page-Table-Walker replays — purely
 * through the virtual-memory interface:
 *
 *   - madvise(MADV_DONTNEED) discards the resident page; the next access faults,
 *     the kernel walks the software page tables, and the hardware Page-Table
 *     Walker repopulates the TLB. Repeating this every iteration "tortures" the
 *     PTW state machine, exactly the targeted weak spot.
 *   - Cross-page-boundary accesses: a single 8-byte unaligned load/store placed
 *     at page_end-4 straddles the boundary (4 bytes in each page), forcing two
 *     TLB lookups from one instruction and stressing the page-boundary /
 *     split-access handling in the MMU. (The previous version wrote two separate
 *     same-page 4-byte values — no single access crossed the boundary, so the
 *     "two TLB lookups in one instruction" target was never actually hit.)
 *   - Unaligned accesses at varying offsets stress the misalignment-handling
 *     path of the load/store unit (which interacts with the MMU for
 *     sub-page addressing).
 *   - Stride access sweeps one cache line per page across the whole 4096-page
 *     region (16 MB, ~64x a typical DTLB), so every pass evicts resident TLB
 *     entries and replays the page-table walker — real TLB thrashing, the
 *     actual "torture the TLB" goal. (The old 64-page working set fit in any
 *     DTLB and never thrashed.)
 *
 * SDC detection: for each page we write a known 64-bit golden pattern, issue
 * MADV_DONTNEED (which must read back as zero after the fault — proving the
 * PTW refilled the page consistently), then write the golden again and read it
 * back — it must match byte-exactly. Cross-page and unaligned reads must also
 * match their written values. Any mismatch means the MMU/PTW/TLB logic
 * silently corrupted the address translation or data — a silent data
 * corruption. Detection is byte-exact (memcmp), not tolerance.
 *
 * ARM64-native (uses ARM64 inline-asm load/store + the linux mmap interface);
 * on non-aarch64 it returns a clean EXIT_SKIP from test_run. The test is wired
 * into tests_set_base so it builds on all arches (x86-64 untouched).
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

// Number of pages in the test region. Sized to EXCEED the DTLB capacity so the
// stride sweep forces TLB misses/evictions at scale — the actual "torture the
// TLB" the test's goal requires. A 64-page (256 KB) working set fits in any
// aarch64 DTLB (typically 48-64 entries for 4K pages), so the old value never
// thrashed. 4096 pages (16 MB) is ~64x a typical DTLB, so every stride pass
// evicts resident entries and replays the page-table walker. Per-thread private
// region; 16 MB * 192 threads = 3 GB, well within this 791 GB host's headroom.
// Must be >= 2 for cross-page access.
static constexpr size_t NUM_PAGES = 4096;

// A 64-bit golden pattern with maximum Hamming distance between successive
// writes, to also toggle the data-path gates (operand-space bonus). We cycle
// through a small high-Hamming table across pages.
static const uint64_t GOLDEN_TABLE[] = {
    0x0000000000000000ULL,
    0xFFFFFFFFFFFFFFFFULL,
    0xAAAAAAAAAAAAAAAAULL,
    0x5555555555555555ULL,
    0x0F0F0F0F0F0F0F0FULL,
    0xF0F0F0F0F0F0F0F0ULL,
};
static constexpr size_t GOLDEN_TABLE_SIZE =
    sizeof(GOLDEN_TABLE) / sizeof(GOLDEN_TABLE[0]);

struct MmuStressData {
    // Nothing shared across threads — each thread allocates its own private
    // mmap region in test_run so that madvise(DONTNEED) on one thread never
    // invalidates a page another thread is mid-test on (the single shared-
    // region design failed under multi-threading because concurrent DONTNEED
    // racing with golden-read produced false "TLB corruption" mismatches).
};

#if defined(__aarch64__)
// ARM64 inline-asm load/store of a 64-bit value. Exercising the actual
// load/store instruction path (rather than a plain dereference) keeps the
// memory-ordering / access-type visible to the toolchain.
static inline void mmu_store64(void *addr, uint64_t val)
{
    __asm__ volatile("str %1, [%0]"
                     : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t mmu_load64(const void *addr)
{
    uint64_t res;
    __asm__ volatile("ldr %0, [%1]"
                     : "=r"(res) : "r"(addr) : "memory");
    return res;
}
#else
static inline void mmu_store64(void *addr, uint64_t val)
{
    std::memcpy(addr, &val, sizeof(val));
}
static inline uint64_t mmu_load64(const void *addr)
{
    uint64_t v;
    std::memcpy(&v, addr, sizeof(v));
    return v;
}
#endif

static int mmu_stress_arm_init(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int mmu_stress_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t page_size = (size_t)ps;
    size_t region_size = NUM_PAGES * page_size;

    // Per-thread private region: no cross-thread madvise races.
    uint8_t *region = static_cast<uint8_t *>(
        mmap(nullptr, region_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (region == MAP_FAILED) {
        log_skip(OSResourceIssueSkipCategory,
                 "mmu_stress_arm: mmap failed: %s", strerror(errno));
        return EXIT_SKIP;
    }

    do {
        bool all_passed = true;

        for (size_t p = 0; p < NUM_PAGES; ++p) {
            uint8_t *page = region + p * page_size;
            uint64_t golden = GOLDEN_TABLE[p % GOLDEN_TABLE_SIZE];
            uint64_t golden_alt = GOLDEN_TABLE[(p + 1) % GOLDEN_TABLE_SIZE];

            // ---- (1) Force a TLB miss + Page-Table-Walker replay.
            // MADV_DONTNEED discards the resident anonymous page; the next
            // access faults and the hardware PTW repopulates the TLB.
            if (madvise(page, page_size, MADV_DONTNEED) != 0) {
                log_warning("mmu_stress_arm: madvise failed page %zu: %s",
                            p, strerror(errno));
                all_passed = false;
                break;
            }

            // After DONTNEED an anonymous page reads back as zero. Read it back
            // (faulting the page in via the PTW) and confirm zero — proves the
            // walker refilled the page consistently.
            // Use the first qword of the page.
            uint64_t zero_check = mmu_load64(page);
            if (zero_check != 0) {
                log_warning("mmu_stress_arm: page %zu read 0x%016" PRIx64
                            " after DONTNEED (expected 0) — TLB/PTW corruption",
                            p, zero_check);
                all_passed = false;
                continue;
            }

            // Write the golden pattern through the freshly-refilled TLB.
            mmu_store64(page, golden);
            uint64_t rd = mmu_load64(page);
            if (rd != golden) {
                log_warning("mmu_stress_arm: page %zu store golden 0x%016" PRIx64
                            " read back 0x%016" PRIx64, p, golden, rd);
                all_passed = false;
                continue;
            }

            // ---- (2) Real cross-page-boundary access. Place a single 8-byte
            // value whose address is page_end-4, so 4 bytes land in page p and
            // 4 bytes in page p+1. A single ldr/str of those 8 bytes straddles
            // the boundary -> one instruction, two TLB lookups, stressing the
            // page-boundary / split-access handling. (The previous version did
            // two separate same-page 4-byte accesses — no single access crossed
            // the boundary, so the dual-TLB-lookup target was never hit.)
            if (p + 1 < NUM_PAGES) {
                uint8_t *boundary = page + page_size - 4;  // 4 bytes in p, 4 in p+1
                uint64_t crossv = golden ^ golden_alt;     // high-Hamming mix
                mmu_store64(boundary, crossv);
                uint64_t crd = mmu_load64(boundary);
                if (crd != crossv) {
                    log_warning("mmu_stress_arm: cross-boundary 8-byte access at "
                                "page %zu/%zu mismatch 0x%016" PRIx64 " want 0x%016" PRIx64,
                                p, p + 1, crd, crossv);
                    all_passed = false;
                }
            }

            // ---- (3) Unaligned access at offsets 1, 2, 3 bytes into the page.
            // Stresses the misalignment / sub-page addressing path that
            // interacts with the MMU.
            for (unsigned off = 1; off <= 3; ++off) {
                uint8_t *uaddr = page + off;
                mmu_store64(uaddr, golden_alt);
                uint64_t urd = mmu_load64(uaddr);
                if (urd != golden_alt) {
                    log_warning("mmu_stress_arm: unaligned store at page %zu "
                                "off %u read back 0x%016" PRIx64 " want 0x%016" PRIx64,
                                p, off, urd, golden_alt);
                    all_passed = false;
                }
            }
        }

        // ---- (4) DTLB-thrash sweep. Touch one cache line per page across the
        // WHOLE 4096-page region (16 MB, ~64x a typical DTLB). Because the
        // working set far exceeds DTLB capacity, every access evicts a
        // resident entry and replays the page-table walker — real TLB
        // thrashing, the "torture the TLB" goal. Write a per-page golden,
        // then reload-and-verify in a second pass (so the verify pass is
        // itself TLB-thrashing, not cache-warm).
        for (size_t p = 0; p < NUM_PAGES; ++p) {
            uint64_t sv = GOLDEN_TABLE[p % GOLDEN_TABLE_SIZE] ^ (p * 0x9E3779B97F4A7C15ULL);
            mmu_store64(region + p * page_size, sv);
        }
        for (size_t p = 0; p < NUM_PAGES; ++p) {
            uint64_t sv = GOLDEN_TABLE[p % GOLDEN_TABLE_SIZE] ^ (p * 0x9E3779B97F4A7C15ULL);
            uint64_t srd = mmu_load64(region + p * page_size);
            if (srd != sv) {
                log_warning("mmu_stress_arm: DTLB-thrash page %zu "
                            "mismatch 0x%016" PRIx64 " want 0x%016" PRIx64,
                            p, srd, sv);
                all_passed = false;
                break;
            }
        }

        if (!all_passed) {
            munmap(region, region_size);
            report_fail_msg("mmu_stress_arm: MMU/TLB/Page-Table-Walker SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    munmap(region, region_size);
    return EXIT_SUCCESS;
}
#else
static int mmu_stress_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 MMU/TLB stress "
             "(madvise/inline-asm load-store) required");
    return EXIT_SKIP;
}
#endif

static int mmu_stress_arm_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mmu_stress_arm,
             "MMU/TLB targeted stress: cross-page/unaligned/forced-fault "
             "(MADV_DONTNEED TLB-miss + PTW replay) accesses to torture the "
             "ARM64 page-table-walker state machine (SDC detection)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mmu_stress_arm_init,
    .test_run = mmu_stress_arm_run,
    .test_cleanup = mmu_stress_arm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
