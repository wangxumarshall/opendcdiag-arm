# OpenDCDiag-ARM

OpenDCDiag-ARM 是一个用于识别 CPU 缺陷与故障的开源工具，从 Intel 的 OpenDCDiag 移植而来。它围绕一套完善的 CPU 测试框架构建，由一系列压力测试用例组成。其核心目标是捕捉**静默数据损坏（Silent Data Corruption, SDC）**——计算不崩溃、却产出错误位的结果，通过与 golden value 的逐字节 `memcmp` 比对来判定硅片是否正常工作。

x86-64 仍为参考架构；本仓库在 ARM64（Kunpeng 920 / 通用 ARMv8.1+）上做了并行移植，保留 x86 路径不动。

## Prebuilt binaries by openEuler version (`third-party/rpms`)

OpenDCDiag-ARM ships **prebuilt `opendcdiag` binaries for every openEuler LTS/SP
release from 20.03 through 24.03**, so you can run the tool on a target machine
without compiling. They live in `third-party/rpms/`, which aggregates three git
submodules — one per major version (split to stay under git's single-pack 2 GB
limit):

| Submodule dir | Remote repo | Releases covered |
|---|---|---|
| `third-party/rpms/openEuler-20.03/` | [opendcdiag-arm-rpm-20.03](https://github.com/wangxumarshall/opendcdiag-arm-rpm-20.03) | 20.03 LTS / SP1 / SP2 / SP3 / SP4 |
| `third-party/rpms/openEuler-22.03/` | [opendcdiag-arm-rpm-22.03](https://github.com/wangxumarshall/opendcdiag-arm-rpm-22.03) | 22.03 LTS / SP1 / SP2 / SP3 / SP4 |
| `third-party/rpms/openEuler-24.03/` | [opendcdiag-arm-rpm-24.03](https://github.com/wangxumarshall/opendcdiag-arm-rpm-24.03) | 24.03 LTS / SP1 / SP2 / SP3 / SP4 (SP3 = build baseline) |

> **Clone with submodules** to fetch the RPM trees:
> ```bash
> git clone --recurse-submodules <repo-url>
> # or, in an existing checkout:
> git submodule update --init --recursive third-party/rpms/
> ```

### Layout of each release

Each release has its own directory under its series, named
`openEuler-<series>LTS[_SPx]` (e.g. `openEuler-24.03LTS_SP3`). The directory holds:

```
third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/
├── *.rpm                          # full dependency RPM tree for that OS version
└── built/                         # ready-to-run artifacts (after packaging)
    ├── opendcdiag                 # stripped binary, built for this exact OS version
    ├── libs/                      # non-system runtime .so it needs (libatomic, toolset stdc++, ...)
    └── run-opendcdiag.sh          # sets LD_LIBRARY_PATH=./libs then execs opendcdiag
```

**24.03 SP3 is the reference/baseline version.** Every other release ships the
*same package-name set* plus its full dependency tree, downloaded against the
24.03 SP3 baseline so results are comparable across versions (see
`scripts/offline-build/download-all-versions.sh`).

### Running the prebuilt binary for your OS version

```bash
# 1. Pick the directory matching your running openEuler version exactly.
#    (SP must match — SP3 binaries don't run cleanly on SP4 and vice versa.)
cd third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/built

# 2. Run via the wrapper (sets LD_LIBRARY_PATH to ./libs automatically):
./run-opendcdiag.sh --list-tests
./run-opendcdiag.sh -e zstd19 -t 5000 -n 1   # deterministic, avoids 192-core ULP flakiness
./run-opendcdiag.sh --quality=0 -e arm64_sdc -t 5000
```

### Common commands (offline-build pipeline)

All multi-version tooling lives in `scripts/offline-build/`. Three of them —
`download-deps.sh`, `install-deps.sh`, `build.sh` — share `_common.sh`, which
detects the running openEuler version (`detect_os_sp`, `detect_os_version_full`)
and **refuses to install across SPs** by stamping/reading a `.os-version` tag
file. The version-gated pipeline is the supported way to (re)build or verify
binaries for a specific OS version.

```bash
cd /path/to/opendcdiag-arm
cd scripts/offline-build/

# --- A. Download RPM trees (on a machine WITH network, same OS version as target) ---
./download-deps.sh                    # one version: detect THIS machine's SP, write .os-version tag
                                       #   → output: ./opendcdiag-rpms/ (full dep tree)
./download-all-versions.sh            # all LTS/SP releases at once vs the 24.03 SP3 baseline
                                       #   → fills third-party/rpms/openEuler-{20,22,24}.03/*
./supplement-20.03-gcc10.sh all       # 20.03 only: pull GCC-10 toolset + meson 0.59 into every 20.03 SP
                                       #   (20.03 ships gcc-7; C++20/23 needs gcc-10 from SP4's repo)

# --- B. Install dependencies offline (on the TARGET machine, NO network) ---
#       Pass the matching release's RPM dir; version is checked against the target machine.
./install-deps.sh ../../third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3

# --- C. Native build on the target (deps already installed) ---
./build.sh                            # meson setup + ninja + smoke check (zstd19 -n 1)

# --- D. Container build per OS version (no host install needed; source mounted read-only) ---
./container-build.sh 24.03 SP3        # → build-out/openEuler-24.03LTS_SP3/opendcdiag
./container-build.sh 22.03 SP3        # 22.03: injects -DOPENEULER_22_03 + C++23 polyfill header
./container-build.sh 20.03 SP4        # 20.03: uses gcc-toolset-10, meson 0.59 source tree

# --- E. Package the built binary into its release's built/ dir ---
./package-built-artifacts.sh 24.03 SP3   # → third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/built/

# --- F. Verify the packaged binary runs in a pristine container (only built/ mounted) ---
./verify-built-pristine.sh 24.03 SP3        # full suite; eigen tests run -n 1
./verify-built-pristine.sh 24.03 SP3 smoke  # quick: --list-tests + zstd19 only

# --- G. Run the full test suite inside the build container ---
./run-full-tests.sh 24.03 SP3              # reuse build-out/ binary
./run-full-tests.sh 24.03 SP3 build         # rebuild first, then test
```

> **Why SP must match.** `glibc-devel` from SP3 carries `Requires: glibc = <sp3-N>`;
> installing it onto an SP4 box forces a downgrade of the protected `glibc` and
> deadlocks. `install-deps.sh` reads the `.os-version` tag stamped at download
> time and rejects the mismatch up front, pointing you to re-download on a
> same-SP machine. This is enforced by `scripts/offline-build/_common.sh`.
>
> **22.03 / 20.03 adaptations are container-only.** Older toolchains (gcc-10,
> meson 0.59/0.54, binutils 2.34) lack C++20/23 features. `container-build.sh`
> mounts the source tree **read-only** and adapts on a throwaway copy:
> `-include` of a `framework/compat/cpp23_polyfill.h` polyfill, CXXFLAGS
> version macros (`OPENEULER_22_03` / `OPENEULER_20_03`), and `sed` patches
> (e.g. `string::contains` → `.find()`, `udf` → `.inst`). The host source tree
> is never modified — the x86 / 24.03 reference stays untouched.

For the full dependency breakdown and pitfalls, see
[docs/offline-build-dependencies.md](docs/offline-build-dependencies.md).

## 构建

### 依赖

#### openEuler（推荐，基准构建平台）
已在 openEuler 24.03 LTS-SP3（Kunpeng 920，aarch64）上构建并验证。
```console
# 安装依赖（需 root）
sudo dnf install -y meson ninja-build gcc g++ cmake boost-devel zlib-devel libzstd-devel gtest-devel
# Eigen 5.0.0+ 必需（仓库自带 third-party/eigen5，见下文）
```

#### Ubuntu
已在 Ubuntu 21.04 / 21.10 上构建。安装依赖：
```console
sudo apt-get install gcc g++ cmake libeigen3-dev libboost-all-dev libzstd-dev zlib1g-dev libgtest-dev meson
```

#### Fedora
已在 Fedora 33 / 34 上构建。安装依赖：
```console
sudo dnf install -y boost-devel eigen3-devel gcc gcc-c++ git gtest-devel meson zlib-devel libzstd-devel
```

### 架构支持

| 架构 | 状态 | 说明 |
|------|------|------|
| ARM64（AArch64） | ✅ 全支持 | Kunpeng 920、通用 ARMv8.1+ |
| x86-64 | 参考架构 | 原始 Intel 代码路径，移植过程中保持不动 |

ARM64 能力覆盖：
- **CPU 特性检测**：FP、NEON、CRC32、Crypto 扩展（AES/SHA）、SVE/SVE2
- **拓扑检测**：ACPI PPTT、sysfs、device tree
- **SDC 检测**：ECC 错误（EDAC）、CRC32/CRC64 校验
- **SIMD 运算**：NEON（128 位）+ 256/512 位仿真
- **RAS/ECC**：EDAC 子系统、ACPI APEI、厂商驱动

### ARM64 原生构建

ARM64 要求 Eigen 5.0.0+（系统自带的 Eigen 3.3.x 在 GCC 12+ 下会编译失败）。仓库已自带 `third-party/eigen5/`，aarch64 构建路径在 `tests/cpu/meson.build` 中直接 `include_directories` 指向它，**无需系统安装 eigen3**。

```console
# 仓库自带 eigen5，PKG_CONFIG_PATH 仅为兼容 x86 路径而保留（aarch64 可省，带上无害）
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
ninja -C builddir
```

若需用自备的 Eigen 5.0.0 源码包，先为其生成 pkg-config 文件：
```console
tar -xjf eigen-5.0.0.tar.bz2
cat > eigen-5.0.0/eigen3.pc << 'EOF'
prefix=/path/to/eigen-5.0.0
exec_prefix=${prefix}
Name: Eigen3
Description: A C++ template library for linear algebra
Version: 5.0.0
Cflags: -I${prefix}
EOF
PKG_CONFIG_PATH=./eigen-5.0.0 meson setup builddir --buildtype=release
ninja -C builddir
```

**Kunpeng 920 优化构建**：
```console
meson setup builddir --buildtype=release -Dkunpeng_optimize=true
ninja -C builddir
```

### 可选：启用 OpenSSL SHA 测试（`openssl_sha`）

`openssl_sha` 通过 OpenSSL 计算 SHA-256/384/512，与 golden value 比对以检测 SDC。它**默认不构建**——OpenSSL 是可选依赖，由 meson 选项 `ssl_link_type`（默认 `none`）控制。

| `ssl_link_type` | 行为 |
|-----------------|------|
| `none`（默认）| 不使用 OpenSSL；二进制中不含 `openssl_sha` |
| `dynamic`       | 构建期链接 `libcrypto`；测试直接调用 |
| `static`        | 同 `dynamic`，但链接静态 `libcrypto` |
| `loaded`        | 运行期 `dlopen()` 加载 `libcrypto`（无构建期依赖） |

```console
# openEuler 安装 OpenSSL 开发包
sudo dnf install -y openssl-devel
# Ubuntu: sudo apt-get install libssl-dev
# Fedora: sudo dnf install -y openssl-devel

# 用 Eigen 5 + 动态链接 OpenSSL 配置
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release \
    -Dssl_link_type=dynamic
ninja -C builddir
./builddir/opendcdiag --list-tests | grep openssl_sha    # 确认已进入测试目录
./builddir/opendcdiag -e openssl_sha -t 5000
```

若 configure 找不到 `libcrypto`，用 `pkg-config --modversion libcrypto` 确认它输出了版本号（由上方的 `openssl-devel` / `libssl-dev` 提供）。

> 若曾以不带 `-Dssl_link_type` 的配置构建过，必须重新配置
> （`meson setup --reconfigure builddir -Dssl_link_type=dynamic`）或新建构建目录——单独跑 `ninja` 不会生效。

## 测试用例与检测能力

OpenDCDiag-ARM 当前 ARM64 构建（Kunpeng 920 / openEuler 24.03 SP3）共编译 **273 个测试用例**，覆盖 CPU 各计算单元与子系统的静默数据损坏（SDC）压力检测。下表按检测域归纳，数量与命名均来自 `./builddir/opendcdiag --quality=-1 -l` 的真实输出。

> 命名说明：许多 ARM64 用例沿用了上游 x86 名字（如 `mesh_upi_avx2_*`、`fma_*_avx512`、`ipsec_*_avx`/`_sse`/`_x86_64`），但实现已落到 **NEON / ARM 原生指令**上（描述含 "(ARM NEON version)" / "(ARM64...)" / "simulated on ARM64"）。命名保留是为了与 x86 参考用例对应，便于跨架构比对结果。

| 检测域 | 代表用例 | 检测能力 |
|--------|----------|----------|
| **内存 / 拷贝** | `memcpy0`..`memcpy_l3_cache_size`、`memcpy_sem`、`memcpy_shuffle`、`memcpy_rewr`、`mem_disambiguation`、`mfence`、`modified_sort`、`memcpy_variations_dyn`、`vmovnt1/2/3*`、`mmu_stress_arm` | L1D/L2/L3 各级缓存带宽压力、store-to-load 转发、跨缓存行一致性与内存序、MMU/TLB 针对性扰动 |
| **缓存 / 互联** | `cachebounce`、`cache_stress_aggressor`、`mesh_upi_{avx,avx2,avx512,sse}_*`（约 27 个） | cache line 弹跳、CLFLUSH 压力、MESH/UPI/环形互联的对称/非对称读写在多核间的协同 |
| **锁 / 原子** | `lock*`、`lockless_cmpxchg{,8b,16b}`、`atomic_simd_{128,256,512}`、`atomic_seq_cst`、`spinlock_*`（共约 32 个） | 锁指令、无锁 cmpxchg、128/256/512 位 SIMD 原子访问、自旋锁各类竞争与跨缓存行/跨 socket 场景（ARM64 atomic builtins） |
| **向量 / SIMD** | `swizzle`、`insert_extract`、`kreg1`..`kreg9`、`gather{,scatter}_*`（NEON 仿真） | NEON 排列/插入抽取、掩码寄存器逻辑（x86 k-reg 在 ARM64 仿真）、gather/scatter 数据通路 |
| **FMA / 浮点** | `fma`、`fma_patterns_avx512_*`、`fma_tail*`、`fmatail*`、`fpu_special_values` | FMA 基础与模式压力、尾数精度（tail precision）穷举、NaN/Inf/非规格化等特殊值经 NEON FMA 后的逐字节 golden 比对 |
| **算术 / 大整数** | `adcx`、`adox`、`adcxlong`、`adcx_adox_interleaved`、`adcx_arm`、`bigint_mulx_arm`、`gmp_big{add,num}`、`operand_space_arm` | 进位链 / 溢出链（ADCX/ADOX）、GMP 大整数乘加、高汉明距离操作数压满加法器/进位链/乘法器门翻转 |
| **CRC / 校验** | `crc32`、`crc32_fixed*`、`isal_crc{32,64}_*`、`zpclmul{,_rep}` | ARM64 `crc32` 指令、isa-l CRC32/CRC64 各标准（IEEE/iSCSI/T10/ECMA/ISO/Jones）、zlib PCLMUL CRC 折叠（NEON 实现） |
| **压缩** | `zlib`/`zlib9`/`zlib1`/`zlib_aaa`/`zfuzz`、`zstd`/`zstd19`/`zstd1`/`zstd_aaa` | zlib / zstd 压缩-解压缩往返、不同压缩级别、高度可压缩数据、fuzz 压力 |
| **线性代数（Eigen）** | `eigen_gemm_*`（7）、`eigen_sparse`、`eigen_svd*`（含 `_cdouble_sve`）、`eigen_svd_jacobi_*` | GEMM 矩阵乘、稀疏 Cholesky 求解、SVD（BDCSVD / Jacobi）对 FMA/向量单元施压；SVE 变体在 SVE 硬件上启用，非 SVE 硬件干净跳过 |
| **IPSec / 密码** | `ipsec_*`（约 46 个，含 AES-CBC/CTR/GCM、HMAC-SHA1/224/256/384/512、XCBC、CMAC、3DES-DOCSIS） | 各类 IPsec 加解密 + 认证组合在 NEON 上的 SDC 检测（命名保留 `_avx`/`_sse`/`_x86_64`/`_avx512` 后缀对应 x86 变体） |
| **OpenSSL SHA** | `openssl_sha` | SHA-256/384/512 经 OpenSSL 计算后与 golden 比对（需 `-Dssl_link_type≠none`） |
| **ARM 加密扩展** | `arm_crypto`（BETA） | ARM AES（AESE/AESMC）crypto 扩展数据通路 SDC 检测 |
| **虚拟化 / 系统寄存器** | `vmx_io_exit`、`vmx_vmexit_*`、`vmxmsr` | guest 触发 vmexit（读 MIDR_EL1/CNTVCT_EL0、dc civac、YIELD 等）的退出路径一致性 |
| **ARM64 SDC 专项** | `arm64_sdc`（BETA）、`power_virus_dit`、`ooo_dep_chain_arm`、`lsu_store_forward_arm`、`l2c_cross_cache_line_arm`、`mmu_split_tlb_arm`、`crt_builtins`、`acl_gemm`、`fisttp_arm` | CRC32/CRC64 校验数据通路；di/dt 电压骤降瞬态 power virus；乱序执行依赖链；LSU store-buffer/load-forwarding 跨界/部分转发；L2 跨缓存行一致性；MMU/TLB/页表遍历器；compiler-rt 软浮点 builtins；Arm Compute Library GEMM/FCVTZS |
| **IST 硬件自检** | `ist`、`ist_array`、`ist_sbaf` | ARM64 In-Silicon Test 硬件自检（**当前为 placeholder**，见下文） |

### 用例质量分级

用例按质量级别（`quality_level`）决定何时运行。**默认为 PROD（2）**，需用 `--quality` 显式下调才能跑更低级别。

| 级别 | 名称 | 含义 | 运行条件 | 本 ARM64 二进制实有数量 |
|------|------|------|----------|----------------------|
| -1 | SKIP | 跳过级 | `quality >= -1` | 5 |
| 0 | BETA | Beta 测试 | `quality >= 0` | 4 |
| 2 | PROD | 生产就绪（默认） | `quality >= 2` | 264 |
| | **合计** | | | **273** |

> 上表数量经实测差分得到：`--list-tests`（PROD）= 264，`--quality=0 --list-tests` = 268，`--quality=-1 --list-tests` = 273。

**BETA（`--quality=0`）4 个**：`arm64_sdc`、`arm_crypto`、`ist_sbaf`、`neon_add`

**SKIP（`--quality=-1`）5 个**：`smi_count`、`eigen_svd_jacobi`、`eigen_svd_jacobi_cdouble`、`eigen_svd_jacobi_double`、`eigen_svd_jacobi_fvectors`

```console
# 只列 PROD（默认）
./builddir/opendcdiag --list-tests
# 列 BETA + PROD
./builddir/opendcdiag --quality=0 --list-tests
# 列全部（含 SKIP）
./builddir/opendcdiag --quality=-1 --list-tests
# 列出用例 + 描述 + 分组
./builddir/opendcdiag -l
# 列出分组（@compression / @ipsec / @math）
./builddir/opendcdiag --list-groups
```

### 占位用例（placeholder）的诚实跳过

移植中暂无法实现的特性，其用例必须返回 `EXIT_SKIP` 并附理由 `"to be implemented (placeholder): <缺失内容>"`，而**不能**返回 `EXIT_SUCCESS` 伪装通过。当前 ARM64 二进制中的 placeholder（实测 `skip-reason`）：

| 用例 | 跳过理由（实测） |
|------|------------------|
| `ist` / `ist_array` | `to be implemented (placeholder): ARM64 In-Silicon Test (IST) backend not yet available; test reserved as the counterpart of Intel IFS` |
| `ist_sbaf`（BETA） | 同上，SBAF 硬件功能自检后端待实现 |
| `smi_count`（SKIP） | `to be implemented (placeholder): SMI counting requires a per-CPU firmware/RAS-interrupt counter not available on this architecture` |

> `mce_check` 是个特例：它是 ARM64 上**真实**的 EDAC 后端测试（统计 `/proc/interrupts` 的 EDAC `ce/ue_count`），实测 `exit: pass`，而非 placeholder。

### 运行测试

```console
./builddir/opendcdiag --list-tests            # 列出默认 PROD 用例
./builddir/opendcdiag -e zstd19 -t 5000       # 跑单个测试，5 秒，全核
./builddir/opendcdiag -e zstd19 -t 5000 -n 1 # 单线程（确定性，规避 192 核 ULP 数值 flakiness）
./builddir/opendcdiag --quality=0 -e arm64_sdc -t 5000   # 跑 BETA 级 ARM64 SDC 用例
./builddir/opendcdiag --quality=-1 -e eigen_svd_jacobi -t 5000  # 跑 SKIP 级用例
./builddir/opendcdiag --dump-cpu-info         # 打印检测到的 CPU + 特性 + 拓扑后退出
./builddir/opendcdiag -s help                 # 列出 RNG 引擎（Constant/LCG/AES）
./builddir/opendcdiag --on-crash=context -e selftest_sigsegv -vv  # 崩溃回溯
```

**Eigen SVD 在 ARM64（NEON + SVE）**：`eigen_svd_cdouble`（复数 double BDCSVD）是 x86 AVX-512 构建的 ARM64 原生对应物，默认跑在 NEON 后端。SVE 硬件（如 Kunpeng 930）另有变体 `eigen_svd_cdouble_sve`（用 Eigen SVE 包后端，aarch64 自动构建）；无 SVE 的硬件（如 Kunpeng 920）在 init 阶段干净跳过（`CpuNotSupported`），不执行任何 SVE 指令。

```console
# NEON 后端（默认 ARM64 构建）
./builddir/opendcdiag --quality=-1 -e eigen_svd_cdouble -t 5000
# 仅 SVE 硬件运行；Kunpeng 920 干净跳过
./builddir/opendcdiag --quality=-1 -e eigen_svd_cdouble_sve -t 5000
```

## 贡献

欢迎为 OpenDCDiag-ARM 贡献代码与提交 pull request。详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 行为准则

OpenDCDiag-ARM 项目采用 Contributor's Covenant 作为 [行为准则][coc]，要求贡献者与使用者在字面与精神上共同遵守。
[coc]: CODE_OF_CONDUCT.md

## 编写测试

OpenDCDiag-ARM 框架旨在让新建 CPU 测试尽可能简单，它处理了大量样板代码：测试生命周期、线程模型、CPU 特性识别、随机数生成等，使测试作者能专注于具体功能。详细指南见 [编写 OpenDCDiag 测试指南](docs/writing_tests.md)。

> 关于离线构建、依赖树与版本管制的完整说明，见 [docs/offline-build-dependencies.md](docs/offline-build-dependencies.md)。
