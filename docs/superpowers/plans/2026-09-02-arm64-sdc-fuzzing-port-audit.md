# arm64-sdc-fuzzing 未提交代码移植审计（结论：已全部移植，无需代码变更）

> **For agentic workers:** 本文档是一次**审计结论**，不是待执行的实施计划。审计于 2026-09-02 完成，所有证据均来自实际命令输出。若未来源目录出现新变更，可基于本文档的比对方法增量重审。

**Goal:** 核实 `/home/sdc/wangxu/arm64-sdc-fuzzing/opendcdiag` 中 framework/ 和 tests/ 的未提交与未跟踪代码是否已完整移植到本代码仓（sdcshield）。

**Architecture:** 不适用（无代码变更）。本仓库是源仓 ARM64 移植工作的下游演进版本，且本仓库版本在每一处差异上都是**更完善的移植**（符合 CLAUDE.md 占位测试诚实性 / x86 不动规则）。

**Tech Stack:** meson + ninja + diff/comm 比对 + sdcshield 实测运行。

## 审计结论（一句话）

**源仓 169 个测试用例全部已存在于本仓库（comm 比对差集为空），且本仓库版本严格优于源仓版本；唯一遗留物是源仓中的编辑器垃圾文件（`spinlock_mini.cpp~`、`.spinlock_mini.cpp.swp`），不应移植。无任何代码需要移植，无任何提交需要产生。**

## 源仓未提交内容清单（实测 `git status --porcelain`）

- 修改（4 个文件）：
  - `framework/sandstone.h` — 注释掉了 `static_assert(NARGS() == 0, "Empty list")`（编译期 hack）
  - `framework/sandstone_test_groups.cpp` / `.h` — 新增 `group_ipsec`
  - `tests/cpu/meson.build` — 新增 13 个测试目录的构建接线 + libisal 依赖
- 未跟踪（13 个测试目录，约 160 个 .cpp / 169 个唯一测试名）：
  `atomic/ cache/ crc/ fma/ gather/ ipsec/ load_port/ lock/ memory/ mesh/ misc/ spinlock/ vector/`

## 逐项差异证据

### 1. 测试名集合比对（核心证据）

```
SRC unique tests: 169   (源仓 13 个目录)
TGT unique tests: 203   (本仓库同目录)
In SRC but NOT in TGT:  (空)
```

本仓库多出的 34 个测试是本仓后续演进新增的（`fpu_special_values`、`zpclmul*`、`memcpy_rewr`、`mmu_stress_arm`、`vmovnt*`、`movbe_dump*`、`mrn_*` 等），与源仓无关。

### 2. 所有"源仓独有行"汇总（对全部 95 个内容有差异的文件做聚合 diff）

源仓独有行**只有**以下几类，没有任何算法/逻辑差异：

| 源仓写法 | 本仓库写法 | 性质 |
|---|---|---|
| `#include <arm_neon.h>`（裸包含，x86 构建会直接失败） | `#ifdef __aarch64__` 包裹 + 非 aarch64 分支返回 `EXIT_SKIP` 占位跳过 | 本仓库符合 CLAUDE.md 占位测试诚实性规则 |
| `.minimum_cpu = cpu_haswell` / `cpu_skylake_avx512`（裸 x86 门控，ARM64 上会全部被拒） | `IPSEC_X86_GATE(cpu_haswell)`（`framework/sandstone_ssl.h` 定义：x86 保留原值、非 x86 展开为 0） | 本仓库让 46 个 ipsec 变体在 ARM64 上真实运行 |
| `__asm__ volatile ("yield")`（裸 ARM 汇编） | `#if defined(__aarch64__) yield / #elif __x86_64__ pause` | 本仓库双架构可移植 |
| `#define SKIP_CODE -255` + 硬编码返回（spinlock_with_hle） | `log_skip(CpuNotSupportedSkipCategory, ...)` + `EXIT_SKIP` | 本仓库使用框架规范 API |
| `//static_assert(NARGS() == 0, ...)`（sandstone.h hack） | 保留真正的 `static_assert`（本仓库 sandstone.h 已大幅演进，含 short-ids 等机制） | 源仓 hack 不应移植 |

### 3. meson.build 接线比对

源仓 meson 改动（把 13 个目录加进 `tests_set_base`/`tests_set_skx`/`tests_set_hsw`，`cpp.find_library('isal')` + `atomic`）在本仓库**全部已实现且更完善**：
- `isal_lib = cpp.find_library('isal', required : false, static : true)` — 实测 `/usr/lib/libisal.so` 存在、无 pkg-config 文件，本仓库的 find_library 方式正确；10 个 `isal_*` 测试实测列出且 `isal_crc_iscsi` 实测 `exit: pass`
- CRC 三个 `crc32*`（ACLE `__crc32b`）在本仓库有专用 `tests_crc_a` 静态库（`-march=armv8.1-a+crc`），源仓没有
- `group_ipsec` 在本仓库 `framework/sandstone_test_groups.{h,cpp}` 已定义（另有源仓没有的 `group_special`）

### 4. 实测验证（本仓库，2026-09-02，Kunpeng 920）

- `ninja -C builddir`（含 `-Dssl_link_type=dynamic`）：**311/311 构建成功，零新增告警**
- `./builddir/sdcshield --list-tests`：**265 个用例**（其中 ipsec 46 个）
- 抽测 8 个代表类别全部 `exit: pass`：`mesh_upi_avx512_sym`、`spinlock_stress`、`isal_crc_iscsi`、`fma`、`atomic_simd_256`、`lockless_cmpxchg16b`、`crc32_fixed`、`kreg1`
- 全量短跑 `./builddir/sdcshield -t 2000 -n 2`：**`exit: pass`，零 fail 零 crash**

## 审计方法记录（供未来增量重审复用）

```bash
SRC=/home/sdc/wangxu/arm64-sdc-fuzzing/opendcdiag; TGT=<本仓库根>
# 1. 测试名差集（应为空）
comm -23 <(grep -rh 'DECLARE_TEST(' $SRC/tests/cpu/{atomic,cache,crc,fma,gather,ipsec,load_port,lock,memory,mesh,misc,spinlock,vector} --include='*.cpp' | sed 's/.*DECLARE_TEST(//;s/[, ].*//' | sort -u) \
         <(grep -rh 'DECLARE_TEST(' $TGT/tests/cpu/{...同目录...} --include='*.cpp' | sed 's/.*DECLARE_TEST(//;s/[, ].*//' | sort -u)
# 2. 逐文件聚合源仓独有行（人工确认无逻辑差异）
for rel in $(cd $SRC && find tests/cpu/{...} -name '*.cpp'); do
  [ -f "$TGT/$rel" ] && diff "$SRC/$rel" "$TGT/$rel" | grep '^<'
done | sort | uniq -c | sort -rn
```

## 决策

- **不产生任何代码提交**（没有可移植的内容；制造空提交违反诚实原则）。
- 源仓的 `spinlock_mini.cpp~` / `.spinlock_mini.cpp.swp` 为编辑器残留，明确**不移植**。
- 本审计文档本身保留在 `docs/superpowers/plans/` 作为"为什么没有产生移植提交"的 provenance 记录。若用户希望将其入库，作为单独的 docs 提交（feature 分支）处理。
