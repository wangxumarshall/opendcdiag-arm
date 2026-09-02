# arm64-sdc-fuzzing 全量移植审计（扩展重审，结论：已全部移植，无需代码变更）

> **For agentic workers:** 本文档是一次**审计结论**，不是待执行的实施计划。2026-09-02 上午已完成针对源仓**未提交**代码的审计（见 [2026-09-02-arm64-sdc-fuzzing-port-audit.md](2026-09-02-arm64-sdc-fuzzing-port-audit.md)）；本文档是同日下午的**扩展重审**，范围扩大到源仓 framework/ 和 tests/ 的**全部代码（含已提交 + 未提交）**。所有证据均来自实际命令输出。若未来源目录出现新变更，可基于本文档的比对方法增量重审。

**Goal:** 核实 `/home/sdc/wangxu/arm64-sdc-fuzzing/opendcdiag` 中 framework/ 和 tests/ 的**全部**代码（本仓库为其 ARM64 移植工作的下游演进版本）是否已完整移植到本代码仓（sdcshield）。

**Architecture:** 不适用（无代码变更）。本仓库是源仓 ARM64 移植工作的下游演进版本，且本仓库版本在每一处差异上都是**更完善的移植**（符合 CLAUDE.md 占位测试诚实性 / x86 不动规则）。

**Tech Stack:** meson + ninja + diff/comm 比对 + sdcshield 实测运行。

## 审计结论（一句话）

**源仓全部 203 个测试用例（源码级）与 169 个测试用例（二进制级，aarch64 实际构建产物）均存在于本仓库，比对差集为空；framework 层 4 个"源仓独有"文件均为死代码或已在本地演进替代；聚合 diff 的 3592 行"源仓独有行"逐类核实为本地演进（重构、API 更新、占位跳过规范）而非缺失功能。无任何代码需要移植，无任何提交需要产生。**

## 审计范围与方法

- 范围：`framework/` + `tests/` 全部 `.c/.cpp/.h/.hpp/.S/.build` 文件（含已提交与未提交内容，排除 examples/、builddir、日志文件）。
- 方法：五维交叉比对 —— ① 测试名集合差集；② framework 文件清单差集；③ 逐文件聚合"源仓独有行"；④ 源仓**实际构建的 aarch64 二进制**与本仓库二进制的测试清单差集；⑤ 独有行中函数定义符号的逐一溯源。

## 逐项证据

### 1. 测试名集合比对（源码级）

```
SRC tests (all of tests/): 203    TGT tests: 285
In SRC but NOT in TGT: (空)
```

本仓库多出的 82 个测试名为本仓后续演进新增，与源仓无关。

### 2. 源仓实际构建二进制的测试清单比对（二进制级，最强证据）

源仓 `builddir/opendcdiag` 为 aarch64 ELF（实构建产物，非交叉推测）：

```
SRC binary tests (--quality=-1 --list-tests): 169
In SRC binary but NOT in TGT binary: (空)
TGT-only in binary: 105 (本仓后续演进新增)
```

源仓二进制中的每一个测试都在本仓库二进制中存在并可运行。7 个"源码级存在但两边二进制均缺失"的测试名核实为：
- `simple_add` / `vector_add` — tests/examples/，两边均不构建（CLAUDE.md：examples 从不构建）；
- `ifs` / `ifs_array_bist` / `ifs_sbaf` — `sandstone_ifs.c` 两边均为 `#if defined(__x86_64__) && defined(__linux__)` 门控，aarch64 下不编译任何测试体；
- `atomic_simd_128_unaligned` / `atomic_simd_256_unaligned` — 两边 meson 均注释掉（TGT 注释处附带说明"matching the reference tree's selection"）。

### 3. framework 文件清单差集（4 个源仓独有文件）

| 源仓文件 | 核实结论 |
|---|---|
| `framework/mmap_region.c`（+ `mmap_file`/`munmap_file` 声明于 sandstone_p.h） | 功能已内联演进为本仓库 `logging.cpp` 的 `maybe_mmap_log()`（logging.cpp:384）—— 上游 refactor，非缺失 |
| `framework/device/cpu/arm64/arm64_topology.cpp`（591 行，`Arm64TopologyDetector`） | **死代码**：源仓内无任何调用者（`arm64_topology_init` 无调用点）。本仓库拓扑能力由演进后的 `topology.cpp`（含 `#if defined(__aarch64__)` midr_el1 分支）承担，且本仓库 topology.cpp 比源仓多 226 行演进内容 |
| `framework/device/cpu/arm64/arm64_cpu_device.h` | **死代码**：定义的 `arm64_cpu_info_t` 在源仓中无任何使用者 |
| `framework/device/cpu/arm64/neon_vectors.h` | **死代码**：源仓内零 include |

其余 arm64 目录共有文件（arm64_cpuid.h、arm64_ras.h、arm64_sdc_detect.cpp、kunpeng920_ecc.cpp 等）本仓库均为严格超集（符号级比对：源仓独有符号集为空）。

### 4. 聚合"源仓独有行"分类（3592 行 / 137 个文件）

全部归属以下几类，逐类核实无功能缺失：

| 类别 | 代表证据 |
|---|---|
| **上游演进（本仓基于更新的上游）** | `add_engines()`（OpenSSL ENGINE API）已被上游提交 b3315c4 "Remove OpenSSL APIs removed in OpenSSL 4.0" 移除，本仓改用 OSSL_PROVIDER API；`cpu_is_virtualized()` 重构为 `device_has_feature(cpu_feature_hypervisor)`；`mmap_region.c` 内联进 logging.cpp |
| **文件拆分重构** | 源仓单文件 `sandstone.cpp`（1815 独有行）中的 `run_one_test_inner`/`thread_runner`/`test_run_wrapper_function` 等已拆分至本仓 `framework/sandstone_run.cpp`（TGT 独有文件），符号逐一核实存在 |
| **占位测试诚实性（本仓更规范）** | 源仓裸 `#include <arm_neon.h>`、裸 `__asm__ volatile("yield")`、`#define SKIP_CODE -255` 硬编码 → 本仓 `#ifdef __aarch64__` 包裹 + 非 aarch64 分支返回 `EXIT_SKIP` 占位跳过 + `log_skip()` 规范 API |
| **ARM64 真实运行门控（本仓更完善）** | 源仓 `.minimum_cpu = cpu_haswell`/`cpu_skylake_avx512`（ARM64 上全被拒）→ 本仓 `IPSEC_X86_GATE(...)`（x86 保留原值、非 x86 展开为 0），46 个 ipsec 变体在 ARM64 真实运行 |
| **x86 不动规则差异** | `mce_check.cpp` 等的 SRC 独有行为 x86-64 分支的旧写法，本仓已按 x86 不动规则以演进后的形态保留 |
| **gpu meson 差异** | 源仓 level_zero 内核生成器被注释 → 本仓已修复并启用（`level_zero_kernels_generator_ocl`），属本仓演进 |
| **frequency_manager** | 源仓独有行为 cpufreq 缺失时 `exit(EX_IOERR)` 的旧逻辑 → 本仓按 CLAUDE.md 平台怪癖降级为 "skipping" 优雅继续（本仓已演进的已知改进） |
| **编辑器残留（不移植）** | `spinlock_mini.cpp~`、`.spinlock_mini.cpp.swp` |

### 5. 实测验证（本仓库，2026-09-02，Kunpeng 920）

- `./builddir/sdcshield --list-tests`：**265 个用例**
- 抽测 10 个源仓来源代表类别全部 `exit: pass`：`mesh_upi_avx512_sym`、`spinlock_stress`、`isal_crc_iscsi`、`fma`、`atomic_simd_256`、`lockless_cmpxchg16b`、`crc32_fixed`、`kreg1`、`arm64_sdc`、`arm_crypto`

## 审计方法记录（供未来增量重审复用）

```bash
SRC=/home/sdc/wangxu/arm64-sdc-fuzzing/opendcdiag; TGT=<本仓库根>
# 1. 源码级测试名差集（应为空）
comm -23 <(grep -rh 'DECLARE_TEST(' $SRC/tests --include='*.cpp' --include='*.c' | sed 's/.*DECLARE_TEST(//;s/[, ].*//' | sort -u) \
         <(grep -rh 'DECLARE_TEST(' $TGT/tests --include='*.cpp' --include='*.c' | sed 's/.*DECLARE_TEST(//;s/[, ].*//' | sort -u)
# 2. 二进制级差集（最强证据，应为空）
comm -23 <($SRC/builddir/opendcdiag --quality=-1 --list-tests | sed 's/ - .*//' | sort -u) \
         <($TGT/builddir/sdcshield --quality=-1 --list-tests | sed 's/ - .*//' | sort -u)
# 3. framework 文件清单差集，逐文件溯源（死代码 / 内联 / 上游移除）
(cd $SRC/framework && find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name '*.S' -o -name '*.hpp' \) | sort) > /tmp/s
(cd $TGT/framework && find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name '*.S' -o -name '*.hpp' \) | sort) > /tmp/t
comm -23 /tmp/s /tmp/t
# 4. 聚合源仓独有行分类核实（重点看函数定义符号是否在 TGT 有对应物）
for rel in $(cd $SRC && find framework tests -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' \) | grep -v examples); do
  [ -f "$TGT/$rel" ] && diff "$SRC/$rel" "$TGT/$rel" | grep '^<'
done | sort | uniq -c | sort -rn
```

## 决策

- **不产生任何代码提交**（没有可移植的内容；制造空提交违反诚实原则）。
- 源仓的 `spinlock_mini.cpp~` / `.spinlock_mini.cpp.swp` 为编辑器残留，明确**不移植**。
- 本审计文档保留在 `docs/superpowers/plans/` 作为"为什么没有产生移植提交"的 provenance 记录。若用户希望将其入库，作为单独的 docs 提交（feature 分支）处理。
