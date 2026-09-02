# CLAUDE.md

This file provides guidance to Claude Code when working with code in this repository.

## What this is

SDCShield is a CPU/system defect-detection tool ("is this silicon working correctly?"). Tests stress compute units (FMA, SIMD, compression, SVD) and compare results against golden values — the whole point is catching **silent data corruption (SDC)**: computations that don't crash but produce wrong bits. Forked from Intel's OpenDCDiag and ported to ARM64 (Kunpeng 920 / generic ARMv8.1+), with x86-64 still the reference architecture.

## Build & run

The central header is `framework/sandstone.h` — the authoritative API reference.

```console
# ARM64 requires Eigen 5.0.0+ (system Eigen 3.3.x breaks on GCC 12+).
# The repo ships third-party/eigen5; create its pkg-config file once:
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
ninja -C builddir

# Default binary: builddir/sdcshield
./builddir/sdcshield --list-tests            # list tests at default PROD quality
./builddir/sdcshield -e zstd19 -t 5000       # run one test, 5s, all CPUs
./builddir/sdcshield -e zstd19 -t 5000 -n 1  # single-threaded (deterministic, avoids big-system eigen numeric flakiness)
./builddir/sdcshield --quality=-1 -e <test>  # run SKIP-level tests too
./builddir/sdcshield --selftests ...         # framework self-tests (cause_sigill, kvm_*, etc.)
./builddir/sdcshield --dump-cpu-info         # detected CPU + features + topology, then exit
./builddir/sdcshield -s help                 # list RNG engines (Constant/LCG/AES)
./builddir/sdcshield --on-crash=context -e selftest_sigsegv -vv  # crash backtrace dump

# OpenSSL SHA test is opt-in (default ssl_link_type=none → not built):
PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir \
    --buildtype=release -Dssl_link_type=dynamic
ninja -C builddir && ./builddir/sdcshield --list-tests | grep openssl_sha
```

After changing meson sources/options: `meson setup --reconfigure builddir ...` then `ninja` (plain ninja won't pick up config changes).

Verbose levels: `-v` (1), `-vv` (2) — crash context dumps and per-thread freq appear at verbose≥2. Logging format default is YAML (`-Dlogging_format=yaml|tap|no_output`).

## Architecture

### Fork model (critical to understand test lifecycle)
Each test runs in a **forked child process**. Flow: parent `run_one_test_inner` → `child_run` → `test_init` (child main thread) → spawns per-CPU worker threads each calling `test_run` via `test_run_wrapper_function` → `test_cleanup`. The parent collects results via `forkfd`. A test that crashes crashes the *child*, which reports back through `CrashContext` (see `framework/sysdeps/unix/child_debug.cpp`). Fork modes (`SandstoneApplication::ForkMode`): `no_fork`, `fork_each_test` (default for non-debug), `exec_each_test` (default for debug). `test_preinit` runs **once in the parent** (and is nulled after first run, so it won't re-run across iterations — use `test_init` for per-iteration skipping).

### Test declaration & the "tests" section
Tests are `struct test` instances placed in a special ELF section (`section("tests")`) via the `DECLARE_TEST` macro. The framework iterates `__start_tests`..`__stop_tests` to find them. `DECLARE_MANUAL_TEST` omits the section attribute (used for special/injected tests like `mce_check`, which is always appended last regardless of selection). Fields: `test_preinit`/`test_init`/`test_run`/`test_cleanup` (function pointers — NULL is allowed and skipped, **except `test_run` which must be non-NULL**), `minimum_cpu` (feature gate), `quality_level`, `groups`, `flags`.

### Quality levels
`TEST_QUALITY_SKIP`(-1) < `BETA`(0) < `PROD`(2, default `--quality=2`). A test runs if `quality_level >= requested_quality`. SKIP tests only run at `--quality=-1`. The framework's "SKIP-skip" guard (`sandstone.cpp:546/559`) skips SKIP tests unless `requested_quality < 0` — so a SKIP-level test with a NULL `test_run` will **crash** under `--quality=-1` unless it self-skips in init (see `mce_check`).

### Cross-architecture portability model
The x86-64 implementation is the reference; ARM64 is a parallel port. Many pieces are gated with `#ifdef __x86_64__` with ARM64 `#elif`/stubs. Key principle from this fork: **placeholder tests for unimplemented-on-ARM features must skip with reason "to be implemented (placeholder)"** (return `EXIT_SKIP` from `test_init`, not `EXIT_SUCCESS` — a no-op returning success is misleading). See `tests/cpu/ist/ist.c`, `tests/common/smi_count/smi_count.cpp`, `tests/common/mce_check/mce_check.cpp`. Real features get real implementations; truly-absent hardware (ARM has no CPU microcode/PPIN/MSR) stays a documented stub, not faked.

### CPU feature detection (config-driven generator pipeline)
`framework/device/cpu/{simd.conf | simd-arm.conf}` → `{x86simd_generate.pl | armsimd_generate.pl}` (Perl, run at meson-configure time) → `builddir/.../cpu_features.h` (generated, **do not edit**). This emits `device_features_t` (128-bit), `cpu_feature_*` constants, `cpu_*` arch macros, `features_string`/`features_indices` (name table), `x86_locators`/`x86_architectures` tables. x86 keys on CPUID leaves; ARM keys on HWCAP bits (`getauxval(AT_HWCAP/HWCAP2)`). `detect_cpu()` (`cpuid_internal.h`) fills the global `device_features`; `device_has_feature(cpu_feature_X)` / `minimum_cpu` gating use it. `dump_device_info()` (`cpu_device.cpp`) and `device_features_to_string()` consume the generated tables. **Both generators emit identical symbol names** (`x86_locators`, etc.) so `cpu_device.cpp`/`cpuid_internal.h` work unchanged across arches.

### Sysdeps layering
`framework/sysdeps/<os>/` (linux/darwin/freebsd/windows) + `framework/sysdeps/unix/` (shared unix). `framework/device/cpu/sysdeps/<os>/` for CPU-specific. `framework/sysdeps/generic/` for non-x86 fallbacks (e.g. `kvm.c` skip stub). Arch gating is **inside files** (`#ifdef __x86_64__`/`__aarch64__`) and **inside meson** (`host_machine.cpu_family()`). The meson build for sysdeps picks files per arch — e.g. `msr.c` is x86-only, `interrupt_monitor.cpp` is x86+aarch64.

### InterruptMonitor / RAS
`framework/interrupt_monitor.hpp` + `sysdeps/linux/interrupt_monitor.cpp`. `InterruptMonitorWorks` is `true` on linux x86-64+aarch64. x86 counts MCE/TRM lines from `/proc/interrupts`; aarch64 counts EDAC `ce_count`+`ue_count` (controller-wide, placed at index 0). `count_smi_events()` uses `read_msr` (x86-only MSR 0x34 — no ARM equivalent, returns nullopt). `mce_check` is a special always-inserted test.

### Tests directory layout
`tests/common/` (arch-agnostic: mce_check, smi_count), `tests/cpu/` (eigen_*, zlib, zstd, ifs, ist, openssl), `tests/{gpu,idxd}/` (only built for `-Ddevice_type=gpu/idxd`), `tests/examples/` (never built — reference only). `tests/cpu/meson.build` uses meson **sourceset** mechanism: `tests_set_base` (all arches), `tests_set_hsw`/`tests_set_skx` (x86 AVX2/AVX512, compiled with `-DEigen=EigenAVX2`/`EigenAVX512` namespace rename so multiple SIMD backends link without symbol clash), `tests_set_sve` (aarch64 SVE, `-DEigen=EigenSVE`).

### RNG
`framework/random.cpp`. Engines: Constant, LCG, AES (the default auto-picks AES when `haveAes()`). AES engine uses `#pragma GCC target` per-arch: x86 `_mm_aesenc_si128`, aarch64 `vaesmcq_u8(vaeseq_u8(...))` via `+crypto`. RNG state is per-thread (`thread_rng` union, 64-byte aligned). The framework **overrides libc `rand`/`random`/`srand`** (they abort for the seed functions).

## Writing tests

See `docs/writing_tests.md`. Minimal pattern:
```c
#include "sandstone.h"
static int my_init(struct test *test) { /* set up golden data */ return EXIT_SUCCESS; }
static int my_run(struct test *test, int cpu) {
    TEST_LOOP(test, N) { /* compute, memcmp_or_fail(actual, golden, len, "...") */ }
    return EXIT_SUCCESS;
}
DECLARE_TEST(my_test, "description")
    .test_init = my_init, .test_run = my_run, .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
```
- `EXIT_SKIP` (-255) from init/run → test skipped (use `log_skip(category, "reason")` first).
- `report_fail()` / `memcmp_or_fail()` are `[[noreturn]]` or abort the thread.
- `TEST_LOOP` is the standard timed loop; `test_time_condition()` checks remaining budget.
- Golden-value comparison is **byte-identical `memcmp`** — floating point must be reproducible (the framework quiets SNaN uniformly in `sandstone_data.cpp` precisely so FP16/BF16 NaN bit patterns match cross-arch).

## Known platform quirks (Kunpeng 920, openEuler 24.03 SP3)

- `sysfs cluster_id` is monotonically increasing (138→654→1170…) and `physical_package_id` is large (36/6378/12720/19062) — a firmware/ACPI-PPTT artifact, not an SDCShield bug; read as-is.
- No `cpufreq` on this board → `--vary-frequency`/`--vary-uncore-frequency` print "skipping" and continue (frequency_manager degrades gracefully; it no longer `exit(EX_IOERR)`).
- No `thermal_zone*` CPU zones (only `cooling_device*`) → thermal throttle is a no-op; on boards that do expose `cpu`/`soc` zones it activates.
- Eigen numerical tests (`eigen_svd_double`, `eigen_sparse`) fail **sporadically** under full-system multi-threading (192 CPUs) due to ULP-level differences in parallel SVD/sparse-solve ordering vs strict `memcmp` golden comparison — single-threaded (`-n 1`) always passes. This is an Eigen/large-core-count limitation, not a port defect; same workload would show it on x86 at high concurrency.

## x86-64 untouched rule

The ARM64 port is additive: prefer `#elif defined(__aarch64__)` branches and per-arch meson guards over modifying x86 logic. The x86-64 paths are the reference and must keep working unchanged. Shared `#ifdef __x86_64__` blocks that need ARM64 should be widened to `#if defined(__x86_64__) || defined(__aarch64__)` only when the code genuinely applies to both.

## Patch discipline (feature/porting/bug/adapter)

This repository enforces a strict one-patch-per-unit workflow. Apply it to **every** change, including ARM64 porting points, feature development, bug fixes, and architecture adapters.

### One patch per unit

Each feature, functionality point, bug, or adaptation point is its own commit. Never bundle unrelated changes into one commit. A "unit" means a single coherent item from a work list (e.g. "#13 uncore frequency exit bug" is one patch; "#12 thermal monitor" is the next). When a task spans several numbered points, solve them **one at a time, sequentially** — finish one (verify → commit → push) before starting the next. Do not parallelize or batch.

### Self-verification before commit (mandatory, 100% real)

After writing code and before committing, the AI **must verify itself** with real commands — no claims based on "it should work" or reading the diff. Specifically:

1. **Build clean**: `ninja -C builddir` must succeed with **zero new errors**. Pre-existing benign warnings (e.g. `sysv_abi ignored`, `[[assume]] ignored`, `-Wrestrict` on libstdc++ string internals) are acceptable; any warning/error introduced by the change is a failure.
2. **Functional verification**: run the actual affected behavior with real commands and capture real output — e.g. `./builddir/sdcshield --dump-cpu-info`, `-e <test>`, `--on-crash=context -e selftest_sigsegv -vv`. Quote the real observed output (a frequency value, a `result: pass`, a crash backtrace) as proof, not a prediction.
3. **Regression check**: run at least one unaffected test (e.g. `zstd19`) and confirm `exit: pass`, zero SIGSEGV, to prove no collateral breakage.
4. **x86-64 non-regression**: the diff must not alter x86 behavior. Confirm by inspection that changes are under `#elif/__aarch64__` or per-arch meson guards, or widening `#ifdef __x86_64__` to `|| __aarch64__` only where genuinely shared.
5. 大颗粒度修改，同步更新readme.md和docs目录下对应文档，确保文档100%准确
Do **not** commit if any of these fail. If a verification step fails, fix and re-verify until it passes. Skipping verification or fabricating results ("assumed to pass") is strictly forbidden — every claim in the commit message must correspond to a command the AI actually ran.

### Auto-push to a non-main branch after verification

Once a patch is committed and verified, **push it automatically to the remote** — do not wait to be asked, and do not push to `main`. Work on a feature branch (e.g. `fix/mce-check-arm64-null-test-run`) and `git push` after each commit. If on `main` when starting work, create/switch to a feature branch first (`git checkout -b <branch>`) before committing.

Commit message must not end with:
```
Co-Authored-By: Claude <noreply@anthropic.com>
```

### Plan-driven workflow (mandatory for every non-trivial change)

All non-trivial work — feature development, porting, refactors, multi-step fixes, anything beyond a single obvious line — **must** be executed via a written plan using the `superpowers:writing-plans` skill, not ad-hoc. "Trivial" means a typo or a one-line obvious fix the change itself describes completely.

1. **Plan first**: before writing any code, invoke `superpowers:writing-plans` and save the plan to `docs/superpowers/plans/YYYY-MM-DD-<feature>.md`. The plan defines one-patch-per-unit decomposition, exact files, real test commands, and per-step checkboxes (`- [ ]`).
2. **Plan == the work list**: each plan task maps to exactly one commit, satisfying "One patch per unit" above. Do not bundle multiple plan tasks into one commit, and do not commit work not in the plan.
3. **Track progress visibly**: implement via `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans`. Check off each `- [ ]` as it completes; the live plan file is the single source of truth for what is done vs pending. If the scope changes mid-execution, edit the plan file first, then proceed.
4. **Verify against the plan, not the diff**: the self-verification above applies per task; a task is not "done" until its plan-specified verification command's real output is quoted and its checkbox is checked.
5. **Provenance**: keep plan files in the repo under `docs/superpowers/plans/` (they document *why* a change was made one unit at a time, complementing git history).

If a request would produce more than one commit, write the plan first. No plan, no code.

### Placeholder-test honesty

When porting a feature that cannot be fully implemented yet (e.g. SMI counting on ARM, IST backend), the test must report a clean skip with reason `"to be implemented (placeholder): <what's missing>"` (return `EXIT_SKIP` from `test_init`, **not** `EXIT_SUCCESS`). A no-op test that returns success is a bug — it falsely reports `pass`. The `mce_check` test, by contrast, is a *real* EDAC-backed test on ARM64 and should `pass`.

### 必须诚实、不能说谎、必须100%服从事实、所有工作和结果必须基于事实并且经过严格的逻辑推理或实证，永远尊重事实、永远真诚。
