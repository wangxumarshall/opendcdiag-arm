# IPSEC Test Suite — ARM64 Porting Proposal

**Status:** RESEARCH / DESIGN document. No source changes proposed here are applied; this is the blueprint for a follow-up patch series.
**Scope:** Port `/tests/cpu/ipsec/` (46 `.cpp` files across 6 subdirs) from the x86-only reference tree (`/home/sdc/opendcdiag`) into the ARM64 baseline (`/home/sdc/opendcdiag-arm`).
**Date:** 2026-08-21

---

## TL;DR — the key finding

**All 46 ipsec test files are portable OpenSSL C. Zero files use raw x86 intrinsics.** The `_sse` / `_avx` / `_avx512` / `_x86_64` filename suffixes describe the OpenSSL *provider's internal* code path (selected by OpenSSL at runtime via its own CPU dispatch), **not** intrinsics in the test source. Every test calls only the `s_EVP_*` / `s_HMAC_*` / `s_CMAC_*` function-pointer wrappers from `framework/sandstone_ssl.h`, which are plain `decltype(&Fn) s_##Fn` indirections over OpenSSL's portable public API.

Verified by exhaustive grep across the whole suite:

```
$ cd /home/sdc/opendcdiag/tests/cpu/ipsec
$ grep -rl '_mm_' --include='*.cpp' .          # x86 SSE/AVX intrinsics
(empty)
$ grep -rl 'immintrin\|nmmintrin\|wmmintrin' --include='*.cpp' .
(empty)
$ grep -rl '__x86_64__' --include='*.cpp' .    # any x86-only #ifdef blocks
(empty)
```

**Implication:** the porting effort is *not* a crypto-intrinsic rewrite. It is a small mechanical enablement — add the `group_ipsec` symbol, wire the files into the aarch64 meson build under `crypto_dep`, and drop/replace the x86 `minimum_cpu` gates that reference symbols absent from the ARM64 generated `cpu_features.h`.

### File split (the number that matters)

| Category | Count | Builds on ARM64 as-is? |
|---|---|---|
| Files with **no `minimum_cpu`** (the `_sse` and `_x86_64` variants) | **22** | **YES** — drop into meson, done |
| Files with `.minimum_cpu = cpu_haswell` (the `_avx` variants) | 16 | No — `cpu_haswell` is undefined on ARM64 (see §4) |
| Files with `.minimum_cpu = cpu_skylake_avx512` (the `_avx512` variants) | 8 | No — same reason |
| **Total** | **46** | **22 immediate, 24 after a one-line edit each** |

So: **0 files need an ARM crypto intrinsic rewrite.** The "ARMv8 `vaeseq_u8` / `sha256h` intrinsic mapping" section (§3 below) is included for completeness and for a *future, optional* "native ARM crypto provider" hardening — it is **not required** to ship the suite.

---

## 1. Inventory of the reference ipsec tests

Reference tree: `/home/sdc/opendcdiag/tests/cpu/ipsec/` — 6 subdirs, 46 `.cpp` files. Every file follows the same skeleton (example: `aes192_gcm/ipsec_aes192_gcm_sse.cpp:1-142`):

```c
#include "sandstone.h"
#if SANDSTONE_SSL_BUILD
#include "sandstone_ssl.h"
... local helper calling s_EVP_* / s_HMAC_* / s_CMAC_* ...
static int X_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_aes_192_gcm && ...) {   // runtime OpenSSL probe
        ... memset_random keys/plaintext ...
        ... compute_golden(); test->data = d; return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available ...");
        return EXIT_SKIP;
    }
}
static int X_run(struct test *test, int cpu) {
    TEST_LOOP(test, 256) { encrypt; memcmp_or_fail; decrypt; memcmp_or_fail; mac; memcmp_or_fail; }
    return EXIT_SUCCESS;
}
DECLARE_TEST(ipsec_..., "Intel IPSEC ...")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init / .test_run / .test_cleanup = ...,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
#else  /* SANDSTONE_SSL_BUILD not set */
DECLARE_TEST(ipsec_..., ...) with init that log_skip("OpenSSL build is not enabled") ... END_DECLARE_TEST
#endif
```

### 1a. OpenSSL cipher primitives used (deduped across all 46 files)

Verified via `grep -hoE 's_EVP_(...)' *.cpp | sort -u`:

| OpenSSL call | Primitive | Used by |
|---|---|---|
| `s_EVP_aes_128_cbc()` | AES-128-CBC | 3des_docsis AES-CMAC/XCBC tests, aes128_cbc suite |
| `s_EVP_aes_128_ecb()` | AES-128-ECB | all XCBC tests (RFC 3566 manual construction, e.g. `aes192_cbc_xcbc_96_sse.cpp:41-50`) |
| `s_EVP_aes_192_cbc()` | AES-192-CBC | aes192_cbc suite |
| `s_EVP_aes_192_ctr()` | AES-192-CTR | aes192_ctr suite (all 7 sha/xcbc variants) |
| `s_EVP_aes_192_gcm()` | AES-192-GCM | aes192_gcm suite (3 files) — uses `EVP_CTRL_GCM_SET_IVLEN` / `EVP_CTRL_GCM_GET_TAG` / `SET_TAG` (`aes192_gcm_sse.cpp:36-66`) |
| `s_EVP_aes_256_cbc()` | AES-256-CBC | aes256_cbc suite |
| `s_EVP_des_ede3_cbc()` | 3DES-CBC (DOCSIS BPI) | all 14 3des_docsis files |
| `s_EVP_sha1()` | HMAC-SHA1 | hmac_sha1 variants |
| `s_EVP_sha224()` / `s_EVP_sha256()` | HMAC-SHA-2 (224/256) | sha2_224/sha2_256/sha224 variants |
| `s_EVP_sha384()` / `s_EVP_sha512()` | HMAC-SHA-2 (384/512) | sha2_384/sha2_512/sha384/sha512 variants |
| `s_CMAC_Init/Update/Final` (via `s_EVP_aes_128_cbc()`) | AES-CMAC | aes_cmac variants (e.g. `3des_docsis_aes_cmac_avx512.cpp:38-45`) |
| (manual `aes_xcbc_96`) | AES-XCBC-MAC-96 (RFC 3566), built on `s_EVP_aes_128_ecb()` | all `_xcbc` variants |

**No `AES_*` direct (low-level) calls.** Everything goes through the `EVP_CIPHER` / `HMAC` / `CMAC` abstractions, which OpenSSL implements portably in C and dispatches to its arch-optimized backends internally (on aarch64 OpenSSL will pick its own `aes-armv64` / `sha256-armv8` assembler implementations automatically).

### 1b. Per-subdir breakdown

| Subdir | Files | Cipher | MAC variants |
|---|---|---|---|
| `3des_docsis/` | 14 | `des_ede3_cbc` | HMAC-SHA1/224/256/384/512, AES-CMAC, AES-XCBC (×2: `_x86_64` no-gate, `_avx512` gate=skx) |
| `aes128_cbc/` | 6 | `aes_128_cbc` | HMAC-SHA1, HMAC-SHA2-224, AES-CMAC (×2: `_sse` no-gate, `_avx` gate=hsw) |
| `aes192_cbc/` | 6 | `aes_192_cbc` | SHA2-384, SHA2-512, XCBC-96 (×2: `_sse`/`_avx`) |
| `aes192_ctr/` | 12 | `aes_192_ctr` | HMAC-SHA1/224/256/384/512, XCBC (×2 each) |
| `aes192_gcm/` | 3 | `aes_192_gcm` | (GCM is self-authenticating; no separate MAC) — `_sse`/`_avx`/`_avx512` |
| `aes256_cbc/` | 5 | `aes_256_cbc` | HMAC-SHA1, SHA2-224, SHA2-256 (the `_avx` set; the `_sse` set has 2 files) |

---

## 2. (For completeness) x86 intrinsic → ARM equivalent mapping

This section is included because the task brief asks for it, but **no test in the suite uses these intrinsics** — the tests are OpenSSL-only. The mapping documents what a *future native-provider* rewrite (bypassing OpenSSL) would look like, and is grounded in the existing ARM crypto reference at `tests/cpu/arm64/crypto_test.cpp` (which *does* use ARM intrinsics directly).

### 2a. AES (block cipher)

| x86 intrinsic (hypothetical) | ARM intrinsic (`<arm_neon.h>`) | HWCAP bit | `-march` target |
|---|---|---|---|
| `_mm_aesenc_si128` | `vaeseq_u8(state, rk)` + `vaesmcq_u8(state)` | `HWCAP` bit 3 = `aes` (`cpu_feature_aes`) | `+crypto` (already in `arm64_cpp_flags`, `tests/cpu/arm64/meson.build:15`) |
| `_mm_aeskeygenassist_si128` | (no ARM equivalent; key schedule done in NEON integer ops — see `crypto_test.cpp:29-33` hand-rolled schedule) | — | — |
| `_mm_clmulepi64_si128` (PCLMULQDQ, for GCM/GHASH) | `vmull_p64` / `vmull_high_p64` (PMULL, GF(2^128) mult) | `HWCAP` bit 4 = `pmull` (`cpu_feature_pmull`) | `+crypto` |
| AVX-512 `_mm512_aesenc_epi128` | ARMv8.4-A VAES on SVE: `vaeseq_u64` (512-bit Z-register) — **not in `<arm_neon.h>`**; needs SVE intrinsics `<arm_sve.h>` | `HWCAP2` bit 2 = `sveaes` (`cpu_feature_sveaes`, requires `sve2`) | `-march=armv8.6-a+sve2-aes` |

Reference impl of the first row is already in-repo: `tests/cpu/arm64/crypto_test.cpp:35-49` (ECB encrypt loop: `vld1q_u8` → `veorq_u8` → 9× `(vaeseq_u8, vaesmcq_u8)` → final `veorq_u8` → `vst1q_u8`).

### 2b. SHA (hash)

| x86 SHA-NI intrinsic | ARM intrinsic | HWCAP bit |
|---|---|---|
| `_mm_sha1msg1_epu32` / `_mm_sha1msg2_epu32` | `vsha1mq_u32` / `vsha1cq_u32` / `vsha1pq_u32` / `vsha1su0q_u32` / `vsha1su1q_u32`; plus `vsha1h_u32` for the init round | `HWCAP` bit 5 = `sha1` (`cpu_feature_sha1`) |
| `_mm_sha256msg1_epu32` / `_mm_sha256msg2_epu32` | `vsha256hq_u32` / `vsha256h2q_u32` / `vsha256su0q_u32` / `vsha256su1q_u32` | `HWCAP` bit 6 = `sha2` (`cpu_feature_sha2`) |
| (no x86 SHA-512 intrinsic) | `vsha512h_u64` / `vsha512h2q_u64` / `vsha512su0q_u64` / `vsha512su1q_u64` | `HWCAP` bit 21 = `sha512` (`cpu_feature_sha512`) — ARMv8.4-A |
| `_mm_sha3_*` (AVX-512 SHA-NI extension, rare) | `vsha3*` / KECCACK intrinsics | `HWCAP` bit 17 = `sha3` (`cpu_feature_sha3`) |

The existing `crypto_test.cpp:51-99` shows a (non-intrinsic-accelerated) SHA-256 reference using plain NEON `veorq_u8` — it does **not** use `vsha256hq_u32`. A native-provider rewrite would replace that loop body with the `vsha256*` family and gate on `device_has_feature(cpu_feature_sha2)`.

### 2c. 3DES

There is **no** ARM cryptographic instruction for 3DES (DES was deprecated in ARMv8.3). The `3des_docsis` tests must stay on OpenSSL's software `EVP_des_ede3_cbc()`. This is fine — OpenSSL's 3DES is portable C. **Do not** attempt a NEON rewrite; there is nothing to accelerate with.

### 2d. Summary: which HWCAP bits gate which primitive

Drawn from `framework/device/cpu/simd-arm.conf:23-43`:

| `cpu_feature_*` | HWCAP | Primitive family |
|---|---|---|
| `cpu_feature_aes` (bit 3) | HWCAP | AES rounds (`vaeseq`/`vaesmcq`) |
| `cpu_feature_pmull` (bit 4) | HWCAP | GF(2^128) PMULL (GCM/GHASH) |
| `cpu_feature_sha1` (bit 5) | HWCAP | SHA-1 (`vsha1*`) |
| `cpu_feature_sha2` (bit 6) | HWCAP | SHA-224/256 (`vsha256*`) |
| `cpu_feature_sha3` (bit 17) | HWCAP | SHA-3 / KECCAK |
| `cpu_feature_sm3` (bit 18) / `cpu_feature_sm4` (bit 19) | HWCAP | Chinese national ciphers (not used by ipsec) |
| `cpu_feature_sha512` (bit 21) | HWCAP | SHA-384/512 (`vsha512*`, ARMv8.4-A) |
| `cpu_feature_sve2` (bit 33) / `cpu_feature_sveaes` (bit 34) / `cpu_feature_svepmull` (bit 35) | HWCAP2 | SVE2 crypto (wide AES/PMULL on Z registers) |

Kunpeng 920 (the fork's target board) advertises: `aes, sha1, sha2, crc32, atomics, fphp, asimdhp, cpuid, asimdrdm, jscvt, fcma, dcpop, asimddp, asimdfhm, ssbs` (per `simd-arm.conf:119`, arch `kunpeng920` = `v8_2` base). So AES + SHA1 + SHA2 are present; **SHA-512 is absent** (needs ARMv8.4-A; `cpu_feature_sha512` bit will be clear). OpenSSL's software SHA-512 still works — it just won't use the ARMv8.4 instruction. This is transparent to the tests because they call `s_EVP_sha512()`.

---

## 3. Feature gating plan

### 3a. The only real gating problem: `minimum_cpu` references x86-only symbols

The generated ARM64 header `builddir/framework/device/cpu/cpu_features.h` defines architecture constants for `cpu_aarch64`, `cpu_v8_1 .. cpu_v8_8`, `cpu_kunpeng920`, `cpu_cortex_a76`, `cpu_neoverse_n1/v1/n2/v2`, etc. (lines 96-199). It does **not** define `cpu_haswell` or `cpu_skylake_avx512`:

```
$ grep -c 'cpu_haswell\|cpu_skylake_avx512' \
    /home/sdc/opendcdiag-arm/builddir/framework/device/cpu/cpu_features.h
0
```

(The existing `framework/selftest.cpp:2615-2660` references `cpu_haswell`/`cpu_skylake_avx512` only inside `#if defined(__x86_64__) && !defined(__clang__)`, so it never compiles on aarch64. The ipsec files have **no** such guard.)

Therefore:

- The **22 `_sse`/`_x86_64` files** (no `minimum_cpu`) compile and run unchanged on arm64.
- The **16 `_avx` files** (`.minimum_cpu = cpu_haswell`) will fail to compile on aarch64 because `cpu_haswell` is an undeclared identifier.
- The **8 `_avx512` files** (`.minimum_cpu = cpu_skylake_avx512`) fail the same way.

### 3b. The fix — two options, recommend (A)

**(A) Drop the gate entirely on arm64** (recommended). The x86 gate existed only to avoid running the AVX/AVX-512 OpenSSL provider on a CPU that lacks those ISAs. On arm64 OpenSSL's own dispatch picks the aarch64 provider regardless; there is no "AVX-512 OpenSSL path" to gate. Drop the `.minimum_cpu = ...` line (or wrap it `#ifdef __x86_64__`).

Pattern (applied per-file to the 24 gated variants):

```c
DECLARE_TEST(ipsec_aes192_gcm_avx512, "Intel IPSEC AES192-GCM AVX512")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_gcm_avx512_init,
    .test_run  = aes_gcm_avx512_run,
    .test_cleanup = aes_gcm_avx512_cleanup,
#ifdef __x86_64__
    .minimum_cpu = cpu_skylake_avx512,   /* preserve x86 gate unchanged */
#endif
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
```

This keeps x86-64 byte-for-byte identical (the `#ifdef __x86_64__` block is the same value, same line) and compiles to no gate on aarch64. This honours the repo's "x86-64 untouched rule" (CLAUDE.md).

**(B) Map to an arm64 arch constant** (not recommended). E.g. `.minimum_cpu = cpu_v8_0` (baseline) or `cpu_kunpeng920`. This would skip the test on a pre-ARMv8.0 target, which is academic since the fork targets ARMv8.1+. Adds a maintenance divergence vs. option (A). Skip.

### 3c. Optional native-provider gating (future, not needed for first port)

If a future patch rewrites the inner crypto loop in raw ARM intrinsics (bypassing OpenSSL), the gating pattern would mirror `tests/cpu/spinlock/spinlock_unaligned.cpp:134`:

```c
DECLARE_TEST(...)
    .minimum_cpu = cpu_feature_aes | cpu_feature_pmull,   /* e.g. for native GCM */
    ...
END_DECLARE_TEST
```

…and the `test_init` would do a runtime `device_has_feature(cpu_feature_aes)` check returning `EXIT_SKIP` with `log_skip(..., "to be implemented (placeholder): native ARM AES provider")` per the placeholder-test honesty rule. **This is out of scope for the first port** — the OpenSSL path already exercises the silicon through OpenSSL's own ARM backend, which is sufficient for SDC detection.

---

## 4. Meson wiring

### 4a. Prerequisite: add `group_ipsec` to the arm64 repo

The arm64 baseline does **not** declare `group_ipsec`:

```
$ grep group_ipsec /home/sdc/opendcdiag-arm/framework/sandstone_test_groups.{h,cpp}
(empty)
```

The reference repo declares it (`framework/sandstone_test_groups.h:20` `extern ... group_ipsec;`, `framework/sandstone_test_groups.cpp:29` `constexpr struct test_group group_ipsec = { TEST_GROUP("ipsec", "Tests that perform ipsec computations") };`). Without it, none of the 46 files link (every `DECLARE_TEST` uses `.groups = DECLARE_TEST_GROUPS(&group_ipsec)`).

**Patch 1 (prerequisite, framework):** copy the two-line `group_ipsec` declaration into `framework/sandstone_test_groups.h` (add `group_ipsec,` to the extern list) and `framework/sandstone_test_groups.cpp` (add the `constexpr` definition). This is arch-agnostic and touches no x86 logic — the symbol already exists in the reference tree, the arm64 fork simply lagged behind.

### 4b. Meson: add an aarch64 ipsec block

The reference `tests/cpu/meson.build:240-303` puts ipsec under three x86 sourcesets: `tests_set_base` (the `_sse`/`_x86_64` files, line 240-267), `tests_set_skx` (the `_avx512` files, line 269-293), `tests_set_hsw` (3 `_avx` files, line 295-302). The arm64 repo's `tests/cpu/meson.build` already has the `SANDSTONE_SSL_BUILD`/`crypto_dep` gating pattern for `openssl/openssl_sha.cpp` (lines 229-243) — mirror it.

**Patch 2 (meson, tests/cpu):** add, immediately after the existing `openssl_sha.cpp` block in `tests/cpu/meson.build`:

```meson
if host_machine.cpu_family() == 'aarch64' and framework_config.get('SANDSTONE_SSL_BUILD') == 1
    tests_set_base.add(
        when : crypto_dep,
        if_true: files(
            # IPSEC suite — portable OpenSSL EVP/HMAC/CMAC. All 46 files are
            # arch-agnostic C; OpenSSL's own runtime dispatch selects the
            # aarch64 aes/sha assembler backend. The _sse/_avx/_avx512 suffixes
            # name the *OpenSSL provider path* on x86, not test-source
            # intrinsics (no _mm_ / immintrin in any file).
            'ipsec/3des_docsis/ipsec_3des_docsis_aes_cmac_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_aes_cmac_avx512.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_aes_xcbc_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_aes_xcbc_avx512.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha1_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha1_avx512.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha224_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha224_avx512.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha256_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha256_avx512.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha384_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha384_avx512.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha512_x86_64.cpp',
            'ipsec/3des_docsis/ipsec_3des_docsis_hmac_sha512_avx512.cpp',
            'ipsec/aes128_cbc/ipsec_aes128_cbc_aes_cmac_sse.cpp',
            'ipsec/aes128_cbc/ipsec_aes128_cbc_aes_cmac_avx.cpp',
            'ipsec/aes128_cbc/ipsec_aes128_cbc_hmac_sha1_sse.cpp',
            'ipsec/aes128_cbc/ipsec_aes128_cbc_hmac_sha1_avx.cpp',
            'ipsec/aes128_cbc/ipsec_aes128_cbc_sha2_224_sse.cpp',
            'ipsec/aes128_cbc/ipsec_aes128_cbc_sha2_224_avx.cpp',
            'ipsec/aes192_cbc/ipsec_aes192_cbc_sha2_384_sse.cpp',
            'ipsec/aes192_cbc/ipsec_aes192_cbc_sha2_384_avx.cpp',
            'ipsec/aes192_cbc/ipsec_aes192_cbc_sha2_512_sse.cpp',
            'ipsec/aes192_cbc/ipsec_aes192_cbc_sha2_512_avx.cpp',
            'ipsec/aes192_cbc/ipsec_aes192_cbc_xcbc_96_sse.cpp',
            'ipsec/aes192_cbc/ipsec_aes192_cbc_xcbc_96_avx.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha1_sse.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha1_avx.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha224_sse.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha224_avx.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha256_sse.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha256_avx.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha384_sse.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha384_avx.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha512_sse.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_hmac_sha512_avx.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_xcbc_sse.cpp',
            'ipsec/aes192_ctr/ipsec_aes192_ctr_xcbc_avx.cpp',
            'ipsec/aes192_gcm/ipsec_aes192_gcm_sse.cpp',
            'ipsec/aes192_gcm/ipsec_aes192_gcm_avx.cpp',
            'ipsec/aes192_gcm/ipsec_aes192_gcm_avx512.cpp',
            'ipsec/aes256_cbc/ipsec_aes256_cbc_hmac_sha1_sse.cpp',
            'ipsec/aes256_cbc/ipsec_aes256_cbc_hmac_sha1_avx.cpp',
            'ipsec/aes256_cbc/ipsec_aes256_cbc_sha2_224_sse.cpp',
            'ipsec/aes256_cbc/ipsec_aes256_cbc_sha2_224_avx.cpp',
            'ipsec/aes256_cbc/ipsec_aes256_cbc_sha2_256_avx.cpp',
        )
    )
endif
```

Notes:
- No separate `-march=armv8.1-a+crc+crypto` is needed for these files (unlike `tests/cpu/arm64/crypto_test.cpp`) because they contain **no** ARM intrinsics — they call OpenSSL. Plain `march_generic_flags` (the default `tests_common_cpp_args`) suffices.
- No `tests_set_hsw`/`tests_set_skx` split is needed on arm64 — those x86 sourcesets exist to compile with `-mavx2`/`-mavx512f` respectively; here OpenSSL handles ISA selection, so all 46 files go into `tests_set_base` under the single `crypto_dep` guard.
- The files themselves are copied verbatim from `/home/sdc/opendcdiag/tests/cpu/ipsec/` into `/home/sdc/opendcdiag-arm/tests/cpu/ipsec/` (the arm64 repo currently has no `ipsec/` dir — confirmed: `ls /home/sdc/opendcdiag-arm/tests/cpu/ipsec/` → "no such directory").

### 4c. Build invocation (post-port)

Per CLAUDE.md, OpenSSL is opt-in (`ssl_link_type=none` default). To build the ported suite:

```console
PKG_CONFIG_PATH=./third-part/eigen5 meson setup --reconfigure builddir \
    --buildtype=release -Dssl_link_type=dynamic
ninja -C builddir
./builddir/opendcdiag --list-tests | grep ipsec          # expect 46 entries
./builddir/opendcdiag -e ipsec_aes192_gcm_sse -t 5000 -n 1   # single-thread, deterministic
```

---

## 5. Effort estimate + per-file work breakdown

### 5a. Classification

Every one of the 46 files falls into the same category: **OpenSSL-portable, builds as-is once the `minimum_cpu` x86 symbols are gated out.** Zero files need an ARM crypto rewrite. Breakdown:

| Class | Count | Action per file |
|---|---|---|
| `OPENSSL_PORTABLE_NO_GATE` — the 22 `_sse`/`_x86_64` files with no `minimum_cpu` | 22 | copy verbatim, wire in meson, done |
| `OPENSSL_PORTABLE_X86_GATE` — the 24 `_avx`/`_avx512` files with `.minimum_cpu = cpu_haswell`/`cpu_skylake_avx512` | 24 | copy, wrap the `.minimum_cpu = ...` line in `#ifdef __x86_64__` (see §3b pattern) |
| `NATIVE_ARM_REWRITE` — files needing raw `vaeseq_u8`/`vsha256h` rewrite | **0** | — |
| `SKIP_NO_ARM_ANALOG` — truly x86-only with no ARM equivalent | **0** | — |

### 5b. Per-file porting table (all 46)

Legend: `as-is` = copy verbatim; `gate` = wrap `minimum_cpu` in `#ifdef __x86_64__` (§3b).

| File | Cipher | MAC | `minimum_cpu` | Action |
|---|---|---|---|---|
| `3des_docsis/ipsec_3des_docsis_aes_cmac_x86_64.cpp` | des_ede3_cbc | AES-CMAC | — | as-is |
| `3des_docsis/ipsec_3des_docsis_aes_cmac_avx512.cpp` | des_ede3_cbc | AES-CMAC | skylake_avx512 | gate |
| `3des_docsis/ipsec_3des_docsis_aes_xcbc_x86_64.cpp` | des_ede3_cbc | AES-XCBC | — | as-is |
| `3des_docsis/ipsec_3des_docsis_aes_xcbc_avx512.cpp` | des_ede3_cbc | AES-XCBC | skylake_avx512 | gate |
| `3des_docsis/ipsec_3des_docsis_hmac_sha1_x86_64.cpp` | des_ede3_cbc | HMAC-SHA1 | — | as-is |
| `3des_docsis/ipsec_3des_docsis_hmac_sha1_avx512.cpp` | des_ede3_cbc | HMAC-SHA1 | skylake_avx512 | gate |
| `3des_docsis/ipsec_3des_docsis_hmac_sha224_x86_64.cpp` | des_ede3_cbc | HMAC-SHA224 | — | as-is |
| `3des_docsis/ipsec_3des_docsis_hmac_sha224_avx512.cpp` | des_ede3_cbc | HMAC-SHA224 | skylake_avx512 | gate |
| `3des_docsis/ipsec_3des_docsis_hmac_sha256_x86_64.cpp` | des_ede3_cbc | HMAC-SHA256 | — | as-is |
| `3des_docsis/ipsec_3des_docsis_hmac_sha256_avx512.cpp` | des_ede3_cbc | HMAC-SHA256 | skylake_avx512 | gate |
| `3des_docsis/ipsec_3des_docsis_hmac_sha384_x86_64.cpp` | des_ede3_cbc | HMAC-SHA384 | — | as-is |
| `3des_docsis/ipsec_3des_docsis_hmac_sha384_avx512.cpp` | des_ede3_cbc | HMAC-SHA384 | skylake_avx512 | gate |
| `3des_docsis/ipsec_3des_docsis_hmac_sha512_x86_64.cpp` | des_ede3_cbc | HMAC-SHA512 | — | as-is |
| `3des_docsis/ipsec_3des_docsis_hmac_sha512_avx512.cpp` | des_ede3_cbc | HMAC-SHA512 | skylake_avx512 | gate |
| `aes128_cbc/ipsec_aes128_cbc_aes_cmac_sse.cpp` | aes_128_cbc | AES-CMAC | — | as-is |
| `aes128_cbc/ipsec_aes128_cbc_aes_cmac_avx.cpp` | aes_128_cbc | AES-CMAC | haswell | gate |
| `aes128_cbc/ipsec_aes128_cbc_hmac_sha1_sse.cpp` | aes_128_cbc | HMAC-SHA1 | — | as-is |
| `aes128_cbc/ipsec_aes128_cbc_hmac_sha1_avx.cpp` | aes_128_cbc | HMAC-SHA1 | haswell | gate |
| `aes128_cbc/ipsec_aes128_cbc_sha2_224_sse.cpp` | aes_128_cbc | HMAC-SHA224 | — | as-is |
| `aes128_cbc/ipsec_aes128_cbc_sha2_224_avx.cpp` | aes_128_cbc | HMAC-SHA224 | haswell | gate |
| `aes192_cbc/ipsec_aes192_cbc_sha2_384_sse.cpp` | aes_192_cbc | HMAC-SHA384 | — | as-is |
| `aes192_cbc/ipsec_aes192_cbc_sha2_384_avx.cpp` | aes_192_cbc | HMAC-SHA384 | haswell | gate |
| `aes192_cbc/ipsec_aes192_cbc_sha2_512_sse.cpp` | aes_192_cbc | HMAC-SHA512 | — | as-is |
| `aes192_cbc/ipsec_aes192_cbc_sha2_512_avx.cpp` | aes_192_cbc | HMAC-SHA512 | haswell | gate |
| `aes192_cbc/ipsec_aes192_cbc_xcbc_96_sse.cpp` | aes_192_cbc | AES-XCBC | — | as-is |
| `aes192_cbc/ipsec_aes192_cbc_xcbc_96_avx.cpp` | aes_192_cbc | AES-XCBC | haswell | gate |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha1_sse.cpp` | aes_192_ctr | HMAC-SHA1 | — | as-is |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha1_avx.cpp` | aes_192_ctr | HMAC-SHA1 | haswell | gate |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha224_sse.cpp` | aes_192_ctr | HMAC-SHA224 | — | as-is |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha224_avx.cpp` | aes_192_ctr | HMAC-SHA224 | haswell | gate |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha256_sse.cpp` | aes_192_ctr | HMAC-SHA256 | — | as-is |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha256_avx.cpp` | aes_192_ctr | HMAC-SHA256 | haswell | gate |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha384_sse.cpp` | aes_192_ctr | HMAC-SHA384 | — | as-is |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha384_avx.cpp` | aes_192_ctr | HMAC-SHA384 | haswell | gate |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha512_sse.cpp` | aes_192_ctr | HMAC-SHA512 | — | as-is |
| `aes192_ctr/ipsec_aes192_ctr_hmac_sha512_avx.cpp` | aes_192_ctr | HMAC-SHA512 | haswell | gate |
| `aes192_ctr/ipsec_aes192_ctr_xcbc_sse.cpp` | aes_192_ctr | AES-XCBC | — | as-is |
| `aes192_ctr/ipsec_aes192_ctr_xcbc_avx.cpp` | aes_192_ctr | AES-XCBC | haswell | gate |
| `aes192_gcm/ipsec_aes192_gcm_sse.cpp` | aes_192_gcm | (GCM tag) | — | as-is |
| `aes192_gcm/ipsec_aes192_gcm_avx.cpp` | aes_192_gcm | (GCM tag) | haswell | gate |
| `aes192_gcm/ipsec_aes192_gcm_avx512.cpp` | aes_192_gcm | (GCM tag) | skylake_avx512 | gate |
| `aes256_cbc/ipsec_aes256_cbc_hmac_sha1_sse.cpp` | aes_256_cbc | HMAC-SHA1 | — | as-is |
| `aes256_cbc/ipsec_aes256_cbc_hmac_sha1_avx.cpp` | aes_256_cbc | HMAC-SHA1 | haswell | gate |
| `aes256_cbc/ipsec_aes256_cbc_sha2_224_sse.cpp` | aes_256_cbc | HMAC-SHA224 | — | as-is |
| `aes256_cbc/ipsec_aes256_cbc_sha2_224_avx.cpp` | aes_256_cbc | HMAC-SHA224 | haswell | gate |
| `aes256_cbc/ipsec_aes256_cbc_sha2_256_avx.cpp` | aes_256_cbc | HMAC-SHA256 | haswell | gate |

### 5c. Effort estimate

- **Framework prerequisite:** 2 lines added to `framework/sandstone_test_groups.{h,cpp}` (declare `group_ipsec`). ~5 minutes.
- **File copy + meson wiring:** copy 46 `.cpp` files verbatim into `/home/sdc/opendcdiag-arm/tests/cpu/ipsec/` preserving subdir layout; add the single meson block (§4b). ~15 minutes.
- **`minimum_cpu` gating edits:** 24 files, one `#ifdef __x86_64__`/`#endif` wrap each (mechanical, identical pattern). ~20 minutes.
- **Verification:** `meson setup --reconfigure -Dssl_link_type=dynamic`, `ninja`, `--list-tests | grep ipsec` (expect 46), run `ipsec_aes192_gcm_sse -n 1 -t 5000` and a 3DES test, confirm `exit: pass`. ~20 minutes.

**Total: ~1 hour for the full 46-file suite.** No ARM intrinsic development required.

---

## 6. Recommendation

### Start with the OpenSSL-portable batch — instant quick win

Ship all 46 files in one logical patch series (the work is small and homogeneous), but if a staged approach is preferred:

1. **Patch 1 (framework):** add `group_ipsec` to `framework/sandstone_test_groups.{h,cpp}`. Unblocks everything. Independent of ssl_link_type.
2. **Patch 2 (tests + meson):** copy the **22 no-gate files** + the meson block for just those 22. Build with `-Dssl_link_type=dynamic`, verify `--list-tests | grep ipsec` shows 22 entries, run `ipsec_aes192_gcm_sse` and `ipsec_3des_docsis_hmac_sha1_x86_64` and confirm `exit: pass`. This is the **quick win** — 22 SDC-detecting tests live on arm64 with zero source edits beyond the copy.
3. **Patch 3 (tests, the 24 gated files):** copy the 16 `_avx` + 8 `_avx512` files, each with the `#ifdef __x86_64__ minimum_cpu #endif` wrap from §3b. Extend the meson block to list them. Verify 46 entries, run one `_avx` and one `_avx512` variant.

### Do NOT pursue a native ARM-crypto rewrite as part of this port

The task brief asks for the x86→ARM intrinsic mapping (`vaeseq_u8`, `vsha256h`, `vmull_p64`, etc.), and §2 above provides it for the record. But the ipsec tests are **not** the right place to use it:

- The tests' purpose is **SDC detection** — they want to exercise the silicon's AES/SHA datapaths end-to-end and check the output bits against a golden value. OpenSSL on aarch64 already routes `EVP_aes_*` / `EVP_sha*` to its hand-tuned `aes-armv64.S` / `sha256-armv8.S` assembler, which uses `AESE`/`AESMC`/`SHA256H` instructions. The silicon gets stressed identically whether the test calls `s_EVP_aes_192_gcm()` or a hand-rolled `vaeseq_u8` loop.
- A native rewrite would duplicate ~3,000 lines of OpenSSL's provider logic (key schedule, GCM GHASH counter mode, CBC chaining, CTR nonce increment, CMAC subkey derivation, XCBC RFC 3566 construction) for zero SDC-coverage gain and significant maintenance cost.
- The existing `tests/cpu/arm64/crypto_test.cpp` already provides a focused native-intrinsic AES/SHA smoke test (`vaeseq_u8`/`vaesmcq_u8` for AES-128-ECB, and a NEON SHA-256) for the case where you specifically want to test the *instruction* rather than the *protocol*. That is the right home for future intrinsic-level additions.

### Reservation / caveat to flag

A native rewrite would be worth considering **only** if a future requirement emerges to stress specifically the **PMULL** (GCM GHASH) datapath or the **ARMv8.4 SHA-512** instructions in isolation, since OpenSSL's provider may not always pick the ARMv8.4 path on parts where it's available. If that need arises, the gating pattern (§3c) and the intrinsic reference (§2) in this doc are the starting point. Until then, the OpenSSL path is strictly superior.

### What this port delivers

46 production-quality SDC-detection tests covering AES-128/192/256 (CBC, CTR, GCM), 3DES-DOCSIS, HMAC-SHA1/224/256/384/512, AES-CMAC, and AES-XCBC — exercising the Kunpeng 920's AES/SHA1/SHA2 crypto units (and OpenSSL's software 3DES/SHA-512) under `TEST_LOOP` timing pressure, with byte-identical `memcmp_or_fail` golden comparison on every iteration. Matches the fork's stated goal (CLAUDE.md: "catching silent data corruption") with no placeholder tests and no faked success.
