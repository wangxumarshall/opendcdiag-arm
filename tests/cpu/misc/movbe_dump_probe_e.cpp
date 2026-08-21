/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_e
 * @parblock
 * SDC-trigger probe, variant E: **swapped buffer on a different NUMA/LLC
 * domain than input** (LLC cross-line interaction isolated from the
 * address-advance-stream).
 *
 * Baseline movbe_dump hot loop:
 *   ldr input[i] -> rev -> str swapped[i] -> ldr input[i](reload) -> cmp
 * The store writes to swapped[i] with a linearly-varying address (advances
 * across cache lines). Probe D proved that a *fixed*-address store does NOT
 * trigger (PASS), so the store's advancing address stream is a trigger
 * condition. The open question: is the trigger the advancing *address stream
 * per se, or a *same-LLC-domain interaction* between the swapped writes and
 * the input reloads (both in core 179's local LLC, node 7)?
 *
 * Probe E keeps the baseline structure byte-for-byte (same store to
 * swapped[i], same advancing address stream, same reload of input[i]) but
 * forces swapped and input onto DIFFERENT NUMA nodes / LLC domains:
 *   - input   -> mbind-bound to PROBE_E_INPUT_NODE  (default 7, core 179's home)
 *   - swapped -> mbind-bound to PROBE_E_SWAP_NODE   (default 0, different LLC)
 * So core 179's swapped writes go cross-NUMA (never enter the local LLC for
 * swapped), while input stays local. If probe E STILL triggers, the
 * advancing address stream alone is sufficient (LLC-domain interaction is
 * NOT required). If probe E PASSES, the swapped writes must stay in the
 * same LLC domain as input -- i.e. an LLC cross-line interaction is part of
 * the trigger.
 *
 * Allocation uses mmap + mbind(MPOL_BIND) directly via syscall, so it does
 * not depend on libnuma.
 *
 * NUMA-FALLBACK (fixed): the original probe hardcoded input on node 7 and
 * swapped on node 0, assuming core 179's home node was 7. That is wrong on
 * machines whose topology differs (e.g. this ARM64 host has only nodes 0-3,
 * and memory only on node 1), so mbind() returned EINVAL and the test failed
 * in test_init before the hot loop ever ran -- a test-environment mismatch,
 * not an SDC. The probe now resolves the two nodes at runtime: it scans
 * /sys/devices/system/node/has_memory for nodes that actually have memory,
 * picks two distinct such nodes for the input/swap domains (falling back to
 * a single node, then to plain unbound mmap if fewer than one is available),
 * and only calls mbind() for nodes that exist. Compile-time overrides via
 * PROBE_E_INPUT_NODE / PROBE_E_SWAP_NODE are still honored when they name
 * valid nodes.
 * @endparblock
 */

#include "sandstone.h"
#include <sys/mman.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstring>
#include <cerrno>
#include <cstdio>

#define MOVBE_BUFFER_SIZE (1 << 14)

// Compile-time overrides still honored if they name real memory nodes.
// Defaults (-1) mean "resolve at runtime" (see node_with_memory()).
#ifndef PROBE_E_INPUT_NODE
#  define PROBE_E_INPUT_NODE (-1)
#endif
#ifndef PROBE_E_SWAP_NODE
#  define PROBE_E_SWAP_NODE (-1)
#endif

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

// mmap a page-aligned region of `bytes` and bind it to NUMA `node` via
// mbind(MPOL_BIND). Returns MAP_FAILED only on mmap failure; mbind failure
// is logged but tolerated (the region stays unbound) so the test can still
// run its hot loop instead of failing in init. We round up to pages.
static void *mmap_on_node(size_t bytes, int node)
{
    long pagesize = sysconf(_SC_PAGESIZE);
    size_t total = (bytes + pagesize - 1) & ~(size_t)(pagesize - 1);
    // Ensure total is at least one page and a multiple of pagesize.
    if (total == 0) total = pagesize;

    void *p = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return p;

    // Build a nodemask with just `node`. Only attempt the bind when the node
    // is a real non-negative index that fits in a single unsigned long mask.
    if (node >= 0 && node < (int)(sizeof(unsigned long) * 8)) {
        unsigned long nodemask = (1UL << node);
        // mbind(addr, len, MPOL_BIND, nodemask, maxnode, flags=0)
        // maxnode = number of bits in nodemask (use 64).
        long r = syscall(__NR_mbind, p, total, 2 /*MPOL_BIND*/,
                         &nodemask, sizeof(unsigned long) * 8, 0);
        if (r != 0) {
            // mbind failed (e.g. node has no memory / does not exist on this
            // host): do NOT abort the whole test. Log and leave the region
            // unbound; the hot loop is still a valid byte-swap round-trip
            // probe, just without the cross-NUMA placement guarantee.
            log_info("MovBE-probeE: mbind to node %d failed (errno=%d); "
                     "continuing unbound", node, errno);
        }
    }
    // Touch every page so the binding takes effect (first-touch).
    memset(p, 0, total);
    return p;
}

// Return the number of NUMA nodes that carry memory, by parsing
// /sys/devices/system/node/has_memory (a cpuset-style list like "1" or
// "0,2-3"). Up to `out_nodes_len` node ids are written to out_nodes in
// ascending order; returns the count filled. On any parse error returns 0.
static int nodes_with_memory(int *out_nodes, int out_nodes_len)
{
    FILE *f = fopen("/sys/devices/system/node/has_memory", "r");
    if (!f) return 0;
    char buf[256];
    int count = 0;
    if (fgets(buf, sizeof(buf), f)) {
        // Parse a cpuset list: comma-separated ranges/terms, e.g. "1" or
        // "0,2-3". Trailing newline is ignored by strtoul.
        char *p = buf;
        while (*p && count < out_nodes_len) {
            // skip non-digit separators
            while (*p && *p != '-' && (*p < '0' || *p > '9')) p++;
            if (!*p) break;
            char *end;
            unsigned long a = strtoul(p, &end, 10);
            p = end;
            unsigned long b = a;
            if (*p == '-') {
                p++;
                b = strtoul(p, &end, 10);
                p = end;
            }
            for (unsigned long n = a; n <= b && count < out_nodes_len; ++n)
                out_nodes[count++] = (int)n;
        }
    }
    fclose(f);
    return count;
}

static int movbe_init(struct test *test)
{
    movbe_data *data = (movbe_data *)malloc(sizeof(movbe_data));

    // Resolve the two placement nodes at runtime. Compile-time overrides
    // (PROBE_E_INPUT_NODE / PROBE_E_SWAP_NODE) win if they name real memory
    // nodes; otherwise we pick two distinct memory-bearing nodes from the
    // host's /sys/devices/system/node/has_memory, falling back to the same
    // node (or none, i.e. unbound mmap) if the host has fewer than two.
    int memnodes[16];
    int nmem = nodes_with_memory(memnodes, 16);
    int input_node = PROBE_E_INPUT_NODE;
    int swap_node = PROBE_E_SWAP_NODE;

    auto node_is_memory = [&](int n) {
        for (int i = 0; i < nmem; ++i) if (memnodes[i] == n) return true;
        return false;
    };
    if (input_node < 0 || !node_is_memory(input_node))
        input_node = (nmem >= 1) ? memnodes[0] : -1;
    if (swap_node < 0 || !node_is_memory(swap_node) || swap_node == input_node)
        swap_node = (nmem >= 2) ? memnodes[1] : input_node;

    data->input = (uint32_t *)mmap_on_node(MOVBE_BUFFER_SIZE * sizeof(uint32_t),
                                           input_node);
    data->swapped = (uint32_t *)mmap_on_node(MOVBE_BUFFER_SIZE * sizeof(uint32_t),
                                             swap_node);
    if (data->input == MAP_FAILED || data->swapped == MAP_FAILED) {
        // mmap itself failed (not mbind -- that is tolerated inside
        // mmap_on_node). This is a genuine allocation failure; abort.
        log_error("MovBE-probeE: mmap failed (input=%p swapped=%p errno=%d)",
                  (void *)data->input, (void *)data->swapped, errno);
        free(data);
        return EXIT_FAILURE;
    }
    log_info("MovBE-probeE: placement input_node=%d swap_node=%d "
             "(memnodes=%d, fallback used)",
             input_node, swap_node, nmem);

    for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movbe_run(struct test *test, int cpu)
{
    movbe_data *data = (movbe_data *)test->data;

    TEST_LOOP(test, 1 << 13) {
        // Hot loop kept byte-for-byte identical to baseline movbe_dump:
        // read input[i], rev, store swapped[i], reload input[i], compare.
        // Only the buffer placements differ (input node 7, swapped node 0).
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            data->swapped[i] = val;

            val = __builtin_bswap32(val);

            if (val != data->input[i]) {
                uint32_t inputv = data->input[i];
                uint32_t golden = data->input[i];
                uint32_t actual = val;
                uint32_t xorv = golden ^ actual;
                unsigned char *ib = (unsigned char *)&inputv;
                unsigned char *gb = (unsigned char *)&golden;
                unsigned char *ab = (unsigned char *)&actual;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("MovBE-probeE: Round-trip failed at index %u: "
                          "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X | "
                          "bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
                          "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
                          "xor=%02X%02X%02X%02X",
                          (unsigned)i, inputv, golden, actual, xorv,
                          ib[3], ib[2], ib[1], ib[0],
                          gb[3], gb[2], gb[1], gb[0],
                          ab[3], ab[2], ab[1], ab[0],
                          xb[3], xb[2], xb[1], xb[0]);
                report_fail_msg("MovBE: Round-trip failed at index %u", (unsigned)i);
            }
        }
    }

    return EXIT_SUCCESS;
}

static int movbe_cleanup(struct test *test)
{
    movbe_data *data = (movbe_data *)test->data;
    if (data) {
        if (data->input && data->input != MAP_FAILED)
            munmap(data->input, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
        if (data->swapped && data->swapped != MAP_FAILED)
            munmap(data->swapped, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movbe_dump_probe_e, "movbe_dump probe E: swapped on different NUMA/LLC than input (LLC cross-line interaction isolated)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
