# SDCShield Rename & Apache-2.0 Compliance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the user-facing project brand from `OpenDCDiag` → `SDCShield` (binary, docs, scripts, CI) while preserving all Intel copyright headers, the internal `sandstone` framework, and git history — producing an openEuler-community-importable, Apache-2.0-compliant repository.

**Architecture:** The rename is config-driven, not code-driven. The binary name flows from `meson_options.txt:executable_name` → `SANDSTONE_EXECUTABLE_NAME` macro → `AbstractLogger::program_version`. So changing ~3 meson lines renames the binary; everything else is string/comment/script/doc replacement. The internal `sandstone` C/C++ symbols, header filenames, ELF section names, and namespace are **never touched** — they are Intel's internal codename, Apache-2.0-licensed code, and user-invisible. Compliance is established by adding a `NOTICE` file and an `Origin` derivation statement while leaving every `Copyright 202x Intel Corporation.` header line untouched.

**Tech Stack:** Meson + ninja (build), Perl (`make-gitid.pl`), bash (offline-build scripts), bats (test harness), Doxygen, GitHub Actions / Gitee (CI), Apache-2.0 (license).

## Global Constraints

These apply to **every** task. Verify each before committing.

1. **License:** Apache License 2.0 (root `LICENSE` is the full text; keep it).
2. **Copyright headers — DO NOT DELETE OR OVERWRITE:** Every original source file begins with `Copyright 202x Intel Corporation.` / `SPDX-License-Identifier: Apache-2.0`. These lines must remain byte-identical. New files you author may add a `Copyright 2026 openEuler Community.` line, but never replace an Intel line. A diff that shows `-Copyright ... Intel` covering the same block as `+Copyright ... openEuler` is a hard rejection.
3. **Naming convention for the new brand:**
   - Display name (docs, project(), Doxyfile, README title): `SDCShield`
   - Binary / executable / lowercase identifiers: `sdcshield`
   - Env-var / macro prefix for new code: `SDCSHIELD_` / `__sdcshield_`
   - The lowercase `opendcdiag` and display `OpenDCDiag` must not survive **except** in descriptive derivation statements of the exact form: `SDCShield is derived from OpenDCDiag ...` (trademark fair-use description).
4. **Internal `sandstone` is frozen:** Do **not** rename any `sandstone` / `Sandstone` / `SANDSTONE_` symbol, header filename (`framework/sandstone*.h`/`.cpp`), macro, namespace, or ELF section. These are Apache-2.0-licensed Intel code and are not user-visible. Touching them is out of scope and violates the x86-64-untouched rule in `CLAUDE.md`.
5. **Git history preserved:** No `git rebase`/squash/filter on Intel commits. One-patch-per-unit per `CLAUDE.md`; each commit carries `Signed-off-by:` (DCO for openEuler import).
6. **x86-64 non-regression:** Any change under a shared `#ifdef __x86_64__` may be widened to `|| defined(__aarch64__)` only where genuinely shared. The rename touches no arch-gated logic, so this is trivially satisfied — but confirm in review.
7. **Build floors:** Eigen 5.0.0+ (`PKG_CONFIG_PATH=./third-party/eigen5`), Meson ≥ 0.56, GCC ≥ 12 (openEuler 24.03 SP3 baseline).
8. **Branch discipline:** All work on branch `rename/sdcshield`. Never commit to `main`. `git push` to remote after each verified commit (per `CLAUDE.md` auto-push rule). Commit message footer must NOT contain `Co-Authored-By: Claude <noreply@anthropic.com>`.
9. **Self-verification is mandatory** (per `CLAUDE.md` §"Self-verification before commit"): every task's commit must be backed by a real command's real output quoted in the message or PR — no "assumed to pass."

---

## File Structure (what changes, by responsibility)

**Compliance documents (new files):**
- `NOTICE` — new; Apache-2.0 §4 NOTICE obligation + derivation statement.
- `docs/OPEN_SOURCE_PROVENANCE.md` — new; openEuler-compliance provenance writeup.
- `README.md` — add `## Origin` section at top (derivation statement).

**Identity source (the 3-line rename that flips the binary):**
- `meson.build` — `project()` name; `make-gitid.pl` match-prefix arg.
- `meson_options.txt` — `executable_name` default.
- `Doxyfile` — `PROJECT_NAME`.

**Generated-identity consumer (no edit needed, but verified):**
- `builddir/.../cpu_features.h` etc. — do not edit.
- `framework/sandstone_config.h.in` — `#mesondefine SANDSTONE_EXECUTABLE_NAME` (the macro *name* stays `SANDSTONE_*` per constraint #4; only the *value* changes via the option).

**Code-level literal replacements (comments + 1 internal symbol):**
- `framework/meson.build`, `framework/sandstone_opts.cpp`, `framework/sandstone.h`, `framework/selftest.cpp`, `framework/compat/cpp23_polyfill.h`, `framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp`.
- `tests/cpu/arithmetic_arm/*.cpp` (7 files), `tests/cpu/memory/{memcpy_rewr.cpp,list.h,strategy_config.h}`, `tests/cpu/eigen_svd/sandstone_eigen_common.h`, `tests/cpu/crc/zpclmul_rep.cpp`, `tests/cpu/arithmetic_arm/meson.build`.

**Infrastructure (file renames + script/CI batch):**
- `docs/opendcdiag-cpu.schema.{json,yaml}` → `docs/sdcshield-cpu.schema.{json,yaml}` (git mv).
- `.gitignore` — log/yaml glob patterns.
- `scripts/offline-build/*.sh` (~14 files) — product names, generated `run-*.sh` filename, env var `OPENDCDIAG_OFFLINE_MIN_OS`.
- `.github/workflows/pr.yaml`, `.github/actions/{build-cpu,build-gpu,build-idxd,build-cpu-win}/action.yaml` — `builddir/opendcdiag` path references.
- `bats/testenv.bash`, `bats/sanity-check/helpers.bash` — `SANDSTONE_BIN` path default.

**Docs (pure text):**
- `docs/writing_tests.md`, `docs/coding_style_guide.md`, `docs/multi-version-build-deploy.md`, `docs/multi-version-build-deploy-usermanual.md`, `docs/offline-build-dependencies.md`, `docs/eigen5-build-fix-proposal.md`, `docs/IPSEC_ARM64_PORTING_PROPOSAL.md`, `docs/cases/**`, report files (`MOVBE_SDC_CORE179_*`, `CORE179_SDC_REPORT_CN.md`, `OFHC_RESEARCH_REPORT_CN.md`).
- `CLAUDE.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `LICENSE.3rdparty` (string only).
- `.github/copilot-instructions.md`, `.github/instructions/*.md`.

**External (not in-repo, manual — tracked as final task):**
- `.gitmodules` (3 submodule URLs), GitHub/Gitee repo rename, ghcr image repo rename.

---

## Task 1: Branch + Compliance Foundation (NOTICE, LICENSE.3rdparty string, README Origin)

**Files:**
- Create: `NOTICE`
- Create: `docs/OPEN_SOURCE_PROVENANCE.md`
- Modify: `LICENSE.3rdparty:1`
- Modify: `README.md` (insert `## Origin` after line 3)

**Interfaces:**
- Consumes: existing `LICENSE` (Apache-2.0 full text — already present, do not touch).
- Produces: `NOTICE` file (referenced by every downstream task's compliance check); `## Origin` section referenced by docs tasks.

- [x] **Step 1: Create branch**

```bash
git checkout -b rename/sdcshield
```

Expected: `Switched to a new branch 'rename/sdcshield'`

- [x] **Step 2: Create `NOTICE` file**

Write exactly this content to `./NOTICE`:

```
SDCShield
Copyright (c) 2026 openEuler Community

This project SDCShield is derived from OpenDCDiag, which contains the
sandstone framework originally developed by Intel Corporation.

Original upstream (Intel):
  https://github.com/intel/OpenDCDiag
ARM64 port source:
  https://github.com/wangxumarshall/opendcdiag-arm
Original License: Apache License 2.0

The original copyright statements of source files shall be retained.
```

- [x] **Step 3: Verify NOTICE present**

```bash
cat NOTICE
```

Expected: the exact block above, ending with `...shall be retained.`

- [x] **Step 4: Modify `LICENSE.3rdparty` line 1 (string only — do not touch forkfd/etc. entries below)**

`LICENSE.3rdparty:1` currently:
```
The OpenDCDiag framework includes source code from the following projects.
```
Change to:
```
The SDCShield framework (derived from OpenDCDiag) includes source code from the following projects.
```

- [x] **Step 5: Add `## Origin` section to `README.md`**

Insert after line 3 (after the first descriptive paragraph), before `## 快速开始`:

```markdown
## Origin

SDCShield is derived from OpenDCDiag (which contains the Intel `sandstone` framework), licensed under the [Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for full derivation and upstream provenance.
```

- [x] **Step 6: Verify the diffs preserve all Intel copyright headers**

```bash
git diff LICENSE.3rdparty README.md
git status --short
```

Expected: `LICENSE.3rdparty` shows only line 1 changed; no Intel copyright line touched. `NOTICE` and `docs/OPEN_SOURCE_PROVENANCE.md` shown as new files.

- [x] **Step 7: Commit**

```bash
git add NOTICE LICENSE.3rdparty README.md docs/OPEN_SOURCE_PROVENANCE.md
git commit -s -m "compliance: add NOTICE, derivation statement, 3rd-party string rename

- Add NOTICE fulfilling Apache-2.0 §4 (no upstream NOTICE existed).
- README: add '## Origin' derivation section.
- LICENSE.3rdparty:1: OpenDCDiag -> SDCShield (derived from OpenDCDiag).
- Intel copyright headers on all source files retained unchanged.

Signed-off-by: wangxu <wangxu@example.com>"
```

> Replace the email with the contributor's actual email used for openEuler CLA/DCO.

---

## Task 2: Identity Source Rename (meson + Doxyfile — the 3-line flip)

**Files:**
- Modify: `meson.build:5` (project name) and `meson.build:~108` area (the literal `'opendcdiag'` arg to make-gitid.pl — exact line verified below)
- Modify: `meson_options.txt:18-19` (`executable_name` default + description)
- Modify: `Doxyfile:7` (`PROJECT_NAME`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `builddir/sdcshield` binary; `SANDSTONE_EXECUTABLE_NAME` macro now expands to `"sdcshield"`; `program_version[]` now `"sdcshield-<gitid>"`. Downstream tasks assume the binary is `sdcshield`.

**Why the `make-gitid.pl` arg matters:** `framework/meson.build` passes the literal string `'opendcdiag'` as the 3rd arg to `make-gitid.pl`, which uses it as a `git describe --match="opendcdiag-*"` prefix (see `make-gitid.pl:21`). No such tags exist today (`git tag` shows only `v0.0`), so it matches nothing — but it must be renamed in parallel for correctness. The arg is a plain string literal, **not** `get_option('executable_name')`.

- [x] **Step 1: `meson.build:5` — project name**

```meson
project(
    'SDCShield',
```

(change `'OpenDCDiag'` → `'SDCShield'`)

- [x] **Step 2: `meson_options.txt:18-19` — executable_name default**

```meson
option('executable_name', type : 'string', value : 'sdcshield',
    description : 'Output name of the main binary (default "sdcshield").')
```

- [x] **Step 3: `framework/meson.build:108` — make-gitid.pl match-prefix arg**

The `command:` array currently has `'opendcdiag',` as a string literal. Change to `'sdcshield',`. Leave the surrounding `'@INPUT0@', '@OUTPUT@',` and `get_option('version_suffix'),` untouched.

```meson
        command: [
            perl,
            '@INPUT0@',
            '@OUTPUT@',
            'sdcshield',
            get_option('version_suffix'),
        ],
```

- [x] **Step 4: `framework/meson.build:113` — the message() string**

```meson
    message('sdcshield: not generating gitid.h -- likely included as subdir')
```

- [x] **Step 5: `Doxyfile:7` — PROJECT_NAME**

```
PROJECT_NAME           = "SDCShield"
```

- [x] **Step 6: Reconfigure and build**

```bash
PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir --buildtype=release
ninja -C builddir
```

Expected: `ninja` succeeds with zero new errors/warnings. Note: meson project name changed → `meson setup --reconfigure` is **required** (plain ninja won't pick up the project-name / option-default changes, per `CLAUDE.md`).

- [x] **Step 7: Verify binary renamed and version string**

```bash
ls builddir/sdcshield && ls builddir/opendcdiag 2>/dev/null || echo "old name gone (expected)"
./builddir/sdcshield --version
```

Expected: `builddir/sdcshield` exists; `builddir/opendcdiag` does not; `--version` prints a line beginning with `sdcshield-`.

- [x] **Step 8: Regression — run one real test single-threaded**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1 -o -
```

Expected: `result: pass`, exit 0, zero SIGSEGV. (Single-thread avoids the documented Eigen ULP flakiness on 192-CPU systems.)

- [x] **Step 9: Verify no Intel copyright header touched**

```bash
git diff meson.build meson_options.txt framework/meson.build Doxyfile
```

Expected: only the string changes above; the `# Copyright 2022 Intel Corporation.` / `SPDX-License-Identifier: Apache-2.0` headers remain.

- [x] **Step 10: Commit + push**

```bash
git add meson.build meson_options.txt framework/meson.build Doxyfile
git commit -s -m "rename: project identity OpenDCDiag -> SDCShield (meson + doxyfile)

- meson.build: project('SDCShield').
- meson_options.txt: executable_name default 'sdcshield'.
- framework/meson.build: make-gitid.pl match-prefix 'opendcdiag' -> 'sdcshield'.
- Doxyfile: PROJECT_NAME 'SDCShield'.

Verified: builddir/sdcshield builds; --version prints sdcshield-<gitid>;
zstd19 -n 1 passes. Intel copyright headers unchanged.

Signed-off-by: wangxu <wangxu@example.com>"
git push -u origin rename/sdcshield
```

---

## Task 3: Code-Level Literal Replacements (framework comments + selftest string + polyfill symbol)

**Files:**
- Modify: `framework/sandstone_opts.cpp:248,1155`
- Modify: `framework/sandstone.h:139`
- Modify: `framework/selftest.cpp:71`
- Modify: `framework/compat/cpp23_polyfill.h:59,69` (and comment lines 4,53,105,113 — descriptive only)
- Modify: `framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp:39`

**Interfaces:**
- Consumes: Task 2 (binary is `sdcshield`).
- Produces: no public-API change. The one renamed C symbol `__opendcdiag_pthread_cond_clockwait` → `__sdcshield_pthread_cond_clockwait` is file-scope (static inline + macro redefine), not exported, ABI-neutral.

- [x] **Step 1: `framework/sandstone_opts.cpp:248` — help text comment**

In the `--max-test-loop-count` help paragraph, change:
```
     different random seeds during the same invocation of opendcdiag. The value
```
to:
```
     different random seeds during the same invocation of sdcshield. The value
```

- [x] **Step 2: `framework/sandstone_opts.cpp:1155` — comment**

```cpp
        // Default options for the simplified SDCShield cmdline
```

- [x] **Step 3: `framework/sandstone.h:139` — doc comment**

```
/// asked to terminate by the SDCShield framework. The second parameter, N, specifies the
```

- [x] **Step 4: `framework/selftest.cpp:71` — exception string (user-visible in crash dumps)**

```cpp
        return "SDCShield C++ selftest exception";
```

- [x] **Step 5: `framework/compat/cpp23_polyfill.h` — internal symbol rename**

Line 59 (function name):
```c
__sdcshield_pthread_cond_clockwait(pthread_cond_t *__restrict __cond,
```
Line 69 (macro):
```c
#    define pthread_cond_clockwait __sdcshield_pthread_cond_clockwait
```
Descriptive comment lines 4, 53, 105, 113 mention "OpenDCDiag" — change those to "SDCShield (derived from OpenDCDiag)" where they describe the project, leaving the technical discussion of `std::barrier`/`pthread_cond` semantics intact.

- [x] **Step 6: `framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp:39` — comment**

```cpp
        // Case of bogus data when, e.g., SDCShield is run unprivileged
```

- [x] **Step 7: Build clean**

```bash
ninja -C builddir
```

Expected: zero new errors. Pre-existing benign warnings (`sysv_abi ignored`, `[[assume]] ignored`, `-Wrestrict` on libstdc++ string internals) are acceptable per `CLAUDE.md`; any warning introduced by this task is a failure.

- [x] **Step 8: Functional verification — crash backtrace path (exercises selftest.cpp string + binary)**

```bash
./builddir/sdcshield --on-crash=context -e selftest_sigsegv -vv
```

Expected: child crashes, a backtrace is dumped; the run reports the crash (exit non-zero from the child, overall run handles it). No assertion that the selftest exception string appears here — it is only thrown by `selftest_*` selftest paths; the point is the build runs.

- [x] **Step 9: Verify no Intel copyright header touched**

```bash
git diff framework/sandstone_opts.cpp framework/sandstone.h framework/selftest.cpp framework/compat/cpp23_polyfill.h framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp | grep -E "^-.*Copyright|^-.*SPDX"
```

Expected: empty output (no removed copyright/SPDX lines).

- [x] **Step 10: Commit + push**

```bash
git add framework/sandstone_opts.cpp framework/sandstone.h framework/selftest.cpp framework/compat/cpp23_polyfill.h framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp
git commit -s -m "rename: framework code literals OpenDCDiag -> SDCShield

- sandstone_opts.cpp: help text + cmdline comment.
- sandstone.h: TEST_LOOP doc comment.
- selftest.cpp: SelftestException::what() string.
- cpp23_polyfill.h: __opendcdiag_pthread_cond_clockwait ->
  __sdcshield_pthread_cond_clockwait (file-scope static inline, ABI-neutral).
- effective_cpu_freq.hpp: comment.

Verified: ninja clean; --on-crash=context -e selftest_sigsegv -vv runs.
Intel copyright headers retained.

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

---

## Task 4: Test-Source Literal Replacements (arithmetic_arm + memory + eigen_svd + crc)

**Files:**
- Modify: `tests/cpu/arithmetic_arm/adox_arm.cpp:10,12`, `adcx_arm.cpp:10`, `adcxlong_arm.cpp:11,13`, `adox_arm.cpp` (header), `bigint_mulx_arm.cpp:11,13`, `fisttp_arm.cpp:12,18`, `adcx_adox_interleaved_arm.cpp:12,14`, `meson.build:141`
- Modify: `tests/cpu/memory/memcpy_rewr.cpp:3,10,12,219,247,488,557`, `list.h:2`, `strategy_config.h:3`
- Modify: `tests/cpu/eigen_svd/sandstone_eigen_common.h:14`
- Modify: `tests/cpu/crc/zpclmul_rep.cpp:87`

**Interfaces:**
- Consumes: Task 2.
- Produces: no test API change; comment/string edits only.

- [x] **Step 1: Run a single sed pass scoped to these files, replacing the project brand in comments**

Use a targeted replacement that converts `Intel OpenDCDiag` / `OpenDCDiag` → `Intel OpenDCDiag` stays where it describes upstream origin, and bare `OpenDCDiag` (the *project* self-reference) → `SDCShield`. To keep this mechanical and auditable, replace only the exact tokens. First, preview:

```bash
grep -rn "OpenDCDiag\|opendcdiag" tests/cpu/arithmetic_arm/ tests/cpu/memory/ tests/cpu/eigen_svd/sandstone_eigen_common.h tests/cpu/crc/zpclmul_rep.cpp
```

Then edit each file's matched lines. Rule:
- Lines describing "Pattern follows Intel OpenDCDiag's ..." → keep `Intel OpenDCDiag` (descriptive, fair use) **but** change the self-referential `OpenDCDiag convention` → `SDCShield convention`.
- `tests/cpu/eigen_svd/sandstone_eigen_common.h:14` URL `https://github.com/opendcdiag/opendcdiag/pull/941` — **keep the URL verbatim** (it points to a real upstream PR; changing it breaks the link). Leave this line untouched.

- [x] **Step 2: `tests/cpu/arithmetic_arm/meson.build:141` comment**

```meson
# objects are linked into the final sdcshield binary. build_by_default:false
```

- [x] **Step 3: Build + run a representative ARM64 test**

```bash
ninja -C builddir
./builddir/sdcshield -e adcx_arm -t 2000 -n 1 -o - 2>&1 | tail -5
```

Expected: `result: pass`, exit 0. (If `adcx_arm` is gated on a feature the host lacks, substitute `zstd19`: `./builddir/sdcshield -e zstd19 -t 2000 -n 1 -o -`.)

- [x] **Step 4: Verify no Intel copyright header touched + the upstream PR URL preserved**

```bash
git diff tests/ | grep -E "^-.*Copyright|^-.*SPDX"
git diff tests/cpu/eigen_svd/sandstone_eigen_common.h
```

Expected: first command empty; second shows line 14 (the PR URL) unchanged.

- [x] **Step 5: Commit + push**

```bash
git add tests/cpu/arithmetic_arm/ tests/cpu/memory/ tests/cpu/eigen_svd/sandstone_eigen_common.h tests/cpu/crc/zpclmul_rep.cpp
git commit -s -m "rename: test-source literals OpenDCDiag -> SDCShield (comments only)

arithmetic_arm/*, memory/*, eigen_svd/sandstone_eigen_common.h, crc/
zpclmul_rep.cpp: self-referential OpenDCDiag -> SDCShield in comments.
Upstream 'Intel OpenDCDiag' descriptive mentions retained; upstream PR
URL in sandstone_eigen_common.h:14 left verbatim. meson.build:141 comment.

Verified: ninja clean; adcx_arm -n 1 passes. Intel copyright headers retained.

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

---

## Task 5: Schema File Rename + .gitignore Glob

**Files:**
- Rename: `docs/opendcdiag-cpu.schema.json` → `docs/sdcshield-cpu.schema.json` (git mv)
- Rename: `docs/opendcdiag-cpu.schema.yaml` → `docs/sdcshield-cpu.schema.yaml` (git mv)
- Modify: both files' `description` field + the yaml `$id` URL
- Modify: `.gitignore` (lines `/opendcdiag-*.log`, `/opendcdiag-*.yaml`)

**Interfaces:**
- Consumes: nothing.
- Produces: renamed schema files referenced by docs. The yaml `$id` currently points to `https://raw.githubusercontent.com/opendcdiag/opendcdiag/.../opendcdiag-cpu.schema.json` — see Step 3 for the URL decision.

- [x] **Step 1: git mv the two files**

```bash
git mv docs/opendcdiag-cpu.schema.json docs/sdcshield-cpu.schema.json
git mv docs/opendcdiag-cpu.schema.yaml docs/sdcshield-cpu.schema.yaml
```

- [x] **Step 2: Update `description` field in both files**

`docs/sdcshield-cpu.schema.json`:
```json
  "description": "SDCShield's output schema for CPUs",
```
`docs/sdcshield-cpu.schema.yaml`:
```yaml
description: SDCShield's output schema for CPUs
```

- [x] **Step 3: Decide the yaml `$id` URL — ASK USER (this is a real decision)**

`docs/sdcshield-cpu.schema.yaml:2` currently:
```yaml
#$id: https://raw.githubusercontent.com/opendcdiag/opendcdiag/refs/heads/main/docs/opendcdiag-cpu.schema.json
```
This is a resolvable schema URL. Options:
- **(A)** Update to the future Gitee/openEuler raw URL `https://gitee.com/openeuler/sdcshield/raw/main/docs/sdcshield-cpu.schema.json` (correct end-state but URL may not resolve until the repo exists at that path).
- **(B)** Update path component only, keep host as-is for now: `https://raw.githubusercontent.com/<user>/sdcshield/refs/heads/main/docs/sdcshield-cpu.schema.json`.
- **(C)** Leave verbatim (points to a now-stale upstream path).

Recommended: **(B)** if the GitHub repo will be renamed to `sdcshield`; switch to the Gitee URL in Task 9 (external) once the Gitee repo exists. **DECIDED (2026-09-01): option B** — use `https://raw.githubusercontent.com/wangxumarshall/sdcshield/refs/heads/main/docs/sdcshield-cpu.schema.json`.

- [x] **Step 4: `.gitignore` — rename the two glob patterns**

```
/sdcshield-*.log
/sdcshield-*.yaml
```

- [x] **Step 5: Verify**

```bash
git status --short
grep -n "sdcshield-\*\|\opendcdiag" .gitignore
ls docs/sdcshield-cpu.schema.json docs/sdcshield-cpu.schema.yaml
```

Expected: two `R` (renamed) entries; no `opendcdiag` left in `.gitignore`; new files exist.

- [x] **Step 6: Commit + push**

```bash
git add docs/sdcshield-cpu.schema.json docs/sdcshield-cpu.schema.yaml docs/opendcdiag-cpu.schema.json docs/opendcdiag-cpu.schema.yaml .gitignore
git commit -s -m "rename: schema files opendcdiag-cpu.schema -> sdcshield-cpu.schema

- git mv docs/{opendcdiag,sdcshield}-cpu.schema.{json,yaml}.
- description field -> SDCShield's.
- $id URL: <chosen option B/C path>.
- .gitignore: /sdcshield-*.log, /sdcshield-*.yaml globs.

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

---

## Task 6: Offline-Build Script Batch Rename

**Files:**
- Modify: `scripts/offline-build/_common.sh` (`OPENDCDIAG_OFFLINE_MIN_OS` var → `SDCSHIELD_OFFLINE_MIN_OS`, both occurrences line 14 + line 42)
- Modify: `scripts/offline-build/{package-release.sh, package-built-artifacts.sh, build.sh, container-build.sh, run-full-tests.sh, build-all.sh, verify-built-pristine.sh, download-deps.sh, install-deps.sh, build-images.sh, run-opendcdiag.sh generation, README.md}`

**Interfaces:**
- Consumes: Task 2 (binary is `sdcshield`).
- Produces: product artifacts named `dist/sdcshield-openEuler-<tag>-<sha8>.tar.gz`, generated launcher `run-sdcshield.sh`, image repo `ghcr.io/<user>/sdcshield-offline`.

**Key non-mechanical points (don't blindly sed):**
1. `package-built-artifacts.sh:159` generates `run-opendcdiag.sh` via heredoc — the **filename** and the **in-script exec line** (`exec "$SCRIPT_DIR/opendcdiag" "$@"`) both change to `run-sdcshield.sh` / `sdcshield`.
2. `_common.sh` env var rename `OPENDCDIAG_OFFLINE_MIN_OS` → `SDCSHIELD_OFFLINE_MIN_OS` is a public-ish interface (consumed by line 42). Rename both atomically.
3. Default output dir `./opendcdiag-rpms` (`download-deps.sh:8,27`, `install-deps.sh:8,27,31`) → `./sdcshield-rpms`. This is a *path* external users pass on the CLI; renaming it changes the documented default. Keep consistent across all 3 scripts.
4. `build-images.sh:38` `GHCR="ghcr.io/${GHCR_USER}/opendcdiag-offline"` → `sdcshield-offline` (image repo — external, rename in Task 9 on ghcr side; here just fix the string).

- [ ] **Step 1: Preview the full surface**

```bash
grep -rn "opendcdiag\|OpenDCDiag\|OPENDCDIAG" scripts/offline-build/
```

- [ ] **Step 2: Apply edits per file**

For each file, replace tokens:
- `opendcdiag` (the binary/launcher name) → `sdcshield`
- `run-opendcdiag.sh` → `run-sdcshield.sh`
- `OpenDCDiag` (descriptive, in comments) → `SDCShield (derived from OpenDCDiag)` on first mention per file, `SDCShield` after
- `OPENDCDIAG_OFFLINE_MIN_OS` → `SDCSHIELD_OFFLINE_MIN_OS` (both lines 14, 42 of `_common.sh`)
- `opendcdiag-rpms` → `sdcshield-rpms`
- `opendcdiag-offline` (image name) → `sdcshield-offline`
- Product tarball name `opendcdiag-${OS_TAG}` / `opendcdiag-openEuler-...` → `sdcshield-...`

Specifically in `package-built-artifacts.sh`:
- Line 159: `cat > "$OUTDIR/run-sdcshield.sh" <<'RUN_EOF'`
- Line 161: `# run-sdcshield.sh — 在目标机上运行随包的 sdcshield 二进制。`
- Line 167: `exec "$SCRIPT_DIR/sdcshield" "$@"`
- Line 169: `chmod +x "$OUTDIR/run-sdcshield.sh"`
- Line 201: `for f in sdcshield run-sdcshield.sh; do`
- Line 215: `sdcshield $OS_TAG`

- [ ] **Step 3: Syntax-check every modified shell script**

```bash
for f in scripts/offline-build/*.sh scripts/offline-build/_common.sh; do
  bash -n "$f" || echo "SYNTAX FAIL: $f"
done
```

Expected: no `SYNTAX FAIL` lines.

- [ ] **Step 4: End-to-end build + package verification (the real proof)**

```bash
# from repo root, deps assumed installed (per scripts/offline-build/README.md)
bash scripts/offline-build/build.sh
bash scripts/offline-build/package-release.sh
ls dist/
```

Expected: a tarball named `dist/sdcshield-openEuler-24.03LTS_SP3-<sha8>.tar.gz` (or the host's SP tag). No file named `opendcdiag-*` in `dist/`.

- [ ] **Step 5: Verify launcher + binary inside the tarball**

```bash
tar tzf dist/sdcshield-openEuler-*.tar.gz | grep -E "sdcshield$|run-sdcshield\.sh"
```

Expected: entries `./sdcshield` and `./run-sdcshield.sh`.

- [ ] **Step 6: Verify no Intel copyright header touched (scripts have none, but check)**

```bash
git diff scripts/ | grep -E "^-.*Copyright|^-.*SPDX"
```

Expected: empty (these scripts are fork-authored, but confirm nothing accidental).

- [ ] **Step 7: Commit + push**

```bash
git add scripts/offline-build/
git commit -s -m "rename: offline-build scripts OpenDCDiag -> SDCShield

- _common.sh: OPENDCDIAG_OFFLINE_MIN_OS -> SDCSHIELD_OFFLINE_MIN_OS.
- package-built-artifacts.sh: generates run-sdcshield.sh (was run-opendcdiag.sh).
- download/install-deps.sh: default dir ./sdcshield-rpms.
- build-images.sh: ghcr image repo sdcshield-offline.
- Product tarball: dist/sdcshield-openEuler-<tag>-<sha8>.tar.gz.

Verified: bash -n all scripts; build.sh + package-release.sh produce
dist/sdcshield-openEuler-*.tar.gz with ./sdcshield + ./run-sdcshield.sh.

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

---

## Task 7: CI + bats Path References

**Files:**
- Modify: `.github/workflows/pr.yaml` (lines 68, 71, 75, 86, 93, 118, 121, 123, 126, 132)
- Modify: `.github/actions/build-cpu/action.yaml` (68, 70, 73, 74)
- Modify: `.github/actions/build-gpu/action.yaml` (68, 70, 73)
- Modify: `.github/actions/build-idxd/action.yaml` (66, 68, 71)
- Modify: `.github/actions/build-cpu-win/action.yaml` (43, 47, 49, 52, 53)
- Modify: `bats/testenv.bash:22`
- Modify: `bats/sanity-check/helpers.bash:127` (comment)

**Interfaces:**
- Consumes: Task 2 (binary `sdcshield`).
- Produces: CI references `builddir/sdcshield` (and `builddir-windows/sdcshield.exe`); bats `SANDSTONE_BIN` default points to `sdcshield`.

**Note on `SANDSTONE_BIN` / `SANDSTONE` env vars:** These are *internal* names (constraint #4 — `sandstone` frozen). Do **not** rename the env var `SANDSTONE_BIN`. Only the **path value** it defaults to changes (`../opendcdiag` → `../sdcshield`).

- [ ] **Step 1: Preview CI surface**

```bash
grep -rn "opendcdiag" .github/ bats/
```

- [ ] **Step 2: Apply path replacements**

In all `.github/workflows/pr.yaml` and `.github/actions/*/action.yaml`:
- `builddir/opendcdiag` → `builddir/sdcshield`
- `builddir-windows/opendcdiag` → `builddir-windows/sdcshield`
- `builddir-windows/opendcdiag.exe` → `builddir-windows/sdcshield.exe`
- comment "confirm opendcdiag runs" → "confirm sdcshield runs"
- `build-cpu-win/action.yaml:49` "confirm OpenDCDiag runs" → "confirm SDCShield runs"

`bats/testenv.bash:22`:
```bash
    SANDSTONE_BIN=$BATS_TEST_COMMONDIR/../sdcshield
```

`bats/sanity-check/helpers.bash:127`:
```bash
        # SDCShield's built-in register dumper is only implemented for x86-64.
```

- [ ] **Step 3: Validate YAML + bats syntax**

```bash
python3 -c "import yaml,sys; [yaml.safe_load(open(f)) for f in sys.argv[1:]]" .github/workflows/pr.yaml .github/actions/*/action.yaml
bash -n bats/testenv.bash bats/sanity-check/helpers.bash
```

Expected: no Python/yaml errors; `bash -n` silent.

- [ ] **Step 4: Run bats sanity locally**

```bash
ninja -C builddir
SANDSTONE_BIN=builddir/sdcshield bats bats/sanity-check/10-yaml-validate.bats
```

Expected: `ok` (pass). (Full bats suite is CI's job; one sanity test proves the `SANDSTONE_BIN` path resolution.)

- [ ] **Step 5: Verify no Intel copyright touched + env-var names unchanged**

```bash
git diff .github/ bats/ | grep -E "^-.*Copyright|^-.*SPDX"
git diff bats/testenv.bash | grep -E "^\+.*SANDSTONE_BIN=|^-.*SANDSTONE_BIN="
```

Expected: first empty; second shows the `=...sdcshield` value changed but `SANDSTONE_BIN` var name preserved.

- [ ] **Step 6: Commit + push**

```bash
git add .github/ bats/
git commit -s -m "rename: CI + bats binary path opendcdiag -> sdcshield

- .github/{workflows/pr.yaml, actions/*/action.yaml}: builddir/sdcshield,
  builddir-windows/sdcshield.exe.
- bats/testenv.bash: SANDSTONE_BIN default path -> ../sdcshield
  (env var name SANDSTONE_BIN unchanged - internal codename frozen).
- bats/sanity-check/helpers.bash: comment.

Verified: yaml.safe_load all CI files; bash -n; bats 10-yaml-validate passes.

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

---

## Task 8: Documentation Batch Rename

**Files:**
- Modify: `README.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md`, `docs/writing_tests.md`, `docs/coding_style_guide.md`, `docs/multi-version-build-deploy.md`, `docs/multi-version-build-deploy-usermanual.md`, `docs/offline-build-dependencies.md`, `docs/eigen5-build-fix-proposal.md`, `docs/IPSEC_ARM64_PORTING_PROPOSAL.md`
- Modify (report files — careful, see rule): `docs/cases/**`, `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT*.md`, `docs/CORE179_SDC_REPORT_CN.md`, `docs/OFHC_RESEARCH_REPORT_CN.md`
- Modify: `.github/copilot-instructions.md`, `.github/instructions/*.md`
- Modify: `LICENSE.3rdparty` (any remaining `OpenDCDiag` string beyond line 1, already done in Task 1 — re-check)

**Interfaces:**
- Consumes: Tasks 1–7 (binary `sdcshield`, launcher `run-sdcshield.sh`, schemas renamed).
- Produces: consistent user-facing docs.

**Doc-edit rules (apply per file):**
1. **Executable/launcher path references** (`builddir/opendcdiag`, `run-opendcdiag.sh`, `./opendcdiag`) → `sdcshield` / `run-sdcshield.sh`. These are actionable and must change.
2. **Project self-identity** ("OpenDCDiag is a ...", "the OpenDCDiag framework") → `SDCShield` (first mention may be `SDCShield (derived from OpenDCDiag)`).
3. **Historical/diagnostic report mentions** in `docs/cases/**` and `docs/*CORE179*` / `MOVBE_*` / `OFHC_*` where `opendcdiag` appears as a *run command in a past diagnosis* — keep the brand in prose where it describes what was *historically run*, but update the command examples to `sdcshield` if a reader would copy-paste them. If unsure, prefer changing the command token and leaving prose `OpenDCDiag` (descriptive fair use).
4. **External links** to upstream `github.com/opendcdiag/opendcdiag` / `github.com/wangxumarshall/opendcdiag-arm` — keep verbatim where they are *real source links* (the arm port repo URL changes only after the GitHub rename in Task 9). `SECURITY.md:3` `https://github.com/opendcdiag/opendcdiag/issues` — **DECIDED (2026-09-01): replace with our repo's issue tracker**: `https://github.com/wangxumarshall/sdcshield/issues`.

- [ ] **Step 1: Preview remaining surface**

```bash
grep -ril "opendcdiag\|OpenDCDiag" --include="*.md" . | grep -v "\.git/"
```

- [ ] **Step 2: Edit `CLAUDE.md`**

The CLAUDE.md lines 19–27, 32, 89, 111 reference `builddir/opendcdiag` and `OpenDCDiag`. Change:
- Line 7 first sentence: keep `Forked from Intel's OpenDCDiag` (descriptive, fair use) but change project self-reference `OpenDCDiag is a CPU/system...` → `SDCShield is a CPU/system...`.
- Lines 19–27, 32: `./builddir/opendcdiag` → `./builddir/sdcshield`.
- Line 89: `not an OpenDCDiag bug` → `not an SDCShield bug`.
- Line 111: `./builddir/opendcdiag` → `./builddir/sdcshield`.

- [ ] **Step 3: Edit `README.md` remaining references**

- Line 1 title `# OpenDCDiag-ARM` → `# SDCShield`.
- Line 3 `OpenDCDiag-ARM is ...` → `SDCShield is ... derived from Intel's OpenDCDiag (ARM64 port).`.
- Lines 15–17 submodule URLs: **leave verbatim** (point to real submodule repos; renamed in Task 9).
- Lines 23–28: `./run-opendcdiag.sh` → `./run-sdcshield.sh`.
- Line 52: `./opendcdiag-rpms/` → `./sdcshield-rpms/`.
- Line 91: `exec run-opendcdiag.sh` → `exec run-sdcshield.sh`.

- [ ] **Step 4: Edit remaining docs per the rules above**

For each file in the list, apply rules 1–4. Use the grep output from Step 1 to drive line-by-line edits.

- [ ] **Step 5: Verify no orphaned actionable `opendcdiag` tokens remain**

```bash
grep -rin "opendcdiag" README.md CLAUDE.md CONTRIBUTING.md SECURITY.md docs/writing_tests.md docs/coding_style_guide.md docs/multi-version-build-deploy.md docs/multi-version-build-deploy-usermanual.md docs/offline-build-dependencies.md .github/copilot-instructions.md
```

Expected: only (a) submodule/upstream repo URLs, (b) the `SECURITY.md` upstream issues URL, or (c) explicit `derived from OpenDCDiag` phrases. No `builddir/opendcdiag`, `run-opendcdiag.sh`, or bare project-self-reference.

- [ ] **Step 6: Commit + push**

```bash
git add README.md CLAUDE.md CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md docs/ .github/copilot-instructions.md .github/instructions/
git commit -s -m "docs: rename OpenDCDiag -> SDCShield across docs/CI-instructions

- README, CLAUDE, CONTRIBUTING, SECURITY, CODE_OF_CONDUCT, writing_tests,
  coding_style_guide, multi-version-build-deploy*, offline-build-deps,
  eigen5-build-fix, IPSEC proposal, cases/**, CORE179/MOVBE/OFHC reports.
- Actionable tokens (builddir/opendcdiag, run-opendcdiag.sh) -> sdcshield.
- Upstream/submodule repo URLs and SECURITY.md Intel issues URL left verbatim.
- 'derived from OpenDCDiag' descriptive phrases retained (fair use).

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

---

## Task 9: External Resources (GitHub/Gitee repo, submodules, ghcr image) + Final Sweep

**Files:**
- Modify: `.gitmodules` (3 submodule URLs)

**Interfaces:**
- Consumes: Tasks 1–8 (repo is otherwise consistent).
- Produces: `.gitmodules` pointing at renamed submodule repos; this is the last in-repo change.

**External prerequisites (manual, outside this repo):**
1. Rename GitHub repo `wangxumarshall/opendcdiag-arm` → `wangxumarshall/sdcshield` (GitHub Settings; old name auto-redirects).
2. Rename 3 submodule repos `opendcdiag-arm-rpm-{20.03,22.03,24.03}` → `sdcshield-rpm-{20.03,22.03,24.03}`.
3. Rename/retag ghcr image repo `opendcdiag-offline` → `sdcshield-offline` (or just repoint `GHCR_USER` path; old tags remain pullable).

- [ ] **Step 1: After the 3 submodule repos are renamed on GitHub, update `.gitmodules`**

```
[submodule "third-party/rpms/openEuler-20.03"]
	path = third-party/rpms/openEuler-20.03
	url = git@github.com:wangxumarshall/sdcshield-rpm-20.03.git
[submodule "third-party/rpms/openEuler-22.03"]
	path = third-party/rpms/openEuler-22.03
	url = git@github.com:wangxumarshall/sdcshield-rpm-22.03.git
[submodule "third-party/rpms/openEuler-24.03"]
	path = third-party/rpms/openEuler-24.03
	url = git@github.com:wangxumarshall/sdcshield-rpm-24.03.git
```

- [ ] **Step 2: Re-sync submodule URLs**

```bash
git submodule sync --recursive
git submodule update --init --recursive
```

Expected: submodules check out from new URLs (GitHub auto-redirects anyway, but the config must reflect the canonical new name).

- [ ] **Step 3: Final repo-wide sweep — confirm only allowed `opendcdiag`/`OpenDCDiag` tokens remain**

```bash
grep -rin "opendcdiag" . 2>/dev/null | grep -v "\.git/" | grep -v "^Binary"
```

Categorize every remaining hit. Allowed survivors:
- `LICENSE.3rdparty` / `NOTICE` / `README` / `docs` `derived from OpenDCDiag` phrases.
- Upstream/submodule repo URLs (if any submodule repo wasn't renamed — flag it).
- `docs/cases/**` historical prose describing a past run.
- `tests/cpu/eigen_svd/sandstone_eigen_common.h:14` upstream PR URL.

Anything else (actionable binary/launcher/script path, project self-reference) is a **gap** → fix in a follow-up edit before committing.

- [ ] **Step 4: Full build + regression from clean**

```bash
rm -rf builddir
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
ninja -C builddir
./builddir/sdcshield --version
./builddir/sdcshield -e zstd19 -t 2000 -n 1 -o - | tail -3
./builddir/sdcshield --dump-cpu-info | head -5
```

Expected: clean build; `--version` → `sdcshield-...`; zstd19 passes; dump-cpu-info prints.

- [ ] **Step 5: DCO check — every commit Signed-off-by**

```bash
git log --grep="Signed-off-by" --oneline rename/sdcshield ^main | wc -l
git log --oneline rename/sdcshield ^main | wc -l
```

Expected: the two counts are equal (every new commit has S-o-b).

- [ ] **Step 6: Commit + push**

```bash
git add .gitmodules
git commit -s -m "rename: submodule URLs opendcdiag-arm-rpm -> sdcshield-rpm

External repos renamed on GitHub (manual):
- wangxumarshall/sdcshield (was opendcdiag-arm)
- sdcshield-rpm-{20.03,22.03,24.03} (were opendcdiag-arm-rpm-*)
- ghcr .../sdcshield-offline (was opendcdiag-offline)

.gitmodules updated; git submodule sync --recursive verified.

Final sweep: only descriptive 'derived from OpenDCDiag' phrases and
upstream repo URLs survive. Clean rebuild: builddir/sdcshield --version
prints sdcshield-<gitid>; zstd19 -n 1 passes; dump-cpu-info ok.

Signed-off-by: wangxu <wangxu@example.com>"
git push
```

- [ ] **Step 7: openEuler import readiness checklist**

- [ ] `NOTICE` present and references Intel upstream + ARM64 port URL.
- [ ] `LICENSE` is full Apache-2.0 (untouched).
- [ ] `LICENSE.3rdparty` retains all 3rd-party entries (forkfd etc.) verbatim.
- [ ] No `Copyright ... Intel` line removed in any file (`git log -p` spot-check).
- [ ] Git history preserved (no squash of Intel commits).
- [ ] Every new commit has `Signed-off-by:` (DCO).
- [ ] openEuler CLA signed (contributor-side, outside repo).
- [ ] `docs/OPEN_SOURCE_PROVENANCE.md` written (Task 1).

---

## Self-Review (ran before finalizing)

**1. Spec coverage** (from the compliance analysis provided):
- Apache-2.0 §1 copyright retention → Global Constraint #2 + every task's "verify no Intel header touched" step. ✓
- Apache-2.0 §2 LICENSE retained → Constraint #1; never edited. ✓
- Apache-2.0 §4 NOTICE obligation → Task 1 Step 2. ✓
- 3rd-party NOTICE (LICENSE.3rdparty) → Task 1 Step 4. ✓
- Derivation marking (README Origin) → Task 1 Step 5. ✓
- Trademark: no `OpenDCDiag` as project/binary name → Tasks 2,5,6,7,8 + Task 9 sweep. ✓
- `sandstone` internal-only, never external brand → Constraint #4; no task renames sandstone. ✓
- DCO Signed-off-by → Constraint #5 + Task 9 Step 5. ✓
- CLA → Task 9 Step 7 checklist. ✓
- Git history preserved → Constraint #5; no rebase/filter task. ✓
- "Don't overwrite Intel copyright with openEuler" → Constraint #2 + per-task grep guard. ✓

**2. Placeholder scan:** Two intentional decision points left for the user (the yaml `$id` URL in Task 5 Step 3, and the `SECURITY.md` issues URL in Task 8 rule 4) — both are genuine decisions needing user input, marked with fill-in blanks, not vague TODOs. No "TBD"/"implement later"/"add error handling" patterns.

**3. Type consistency:** `__sdcshield_pthread_cond_clockwait` (Task 3) is the only new symbol; referenced consistently. `SDCSHIELD_OFFLINE_MIN_OS` (Task 6) used at both line 14 and 42 of `_common.sh`. Binary name `sdcshield` used consistently across Tasks 2–9. Launcher `run-sdcshield.sh` consistent in Tasks 6, 8. Env var `SANDSTONE_BIN` deliberately **not** renamed (constraint #4) — consistent in Task 7.

---

*Plan author note: Two real decisions need your input before execution — (a) the schema yaml `$id` URL strategy (Task 5 Step 3), and (b) the `SECURITY.md` upstream-issues URL (Task 8 rule 4). Everything else is fully specified.*
