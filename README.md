# SDCShield

SDCShield is an open-source project designed to identify defects and bugs in ARM CPUs derived from Intel's OpenDCDiag (ARM64 port). It consists of a set of tests built around a sophisticated CPU testing framework.

## Origin

SDCShield is derived from OpenDCDiag (which contains the Intel `sandstone` framework), licensed under the [Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for full derivation and upstream provenance.

## 快速开始

两条入口，按需选一。

### 直接运行预构建二进制（不想编译）

`third-party/rpms/` 聚合了三个 git 子模块，按 openEuler 大版本分仓，覆盖各 LTS/SP 版本的预构建二进制：

| 子模块目录 | 覆盖版本 |
|---|---|
| [`third-party/rpms/openEuler-20.03/`](https://github.com/wangxumarshall/opendcdiag-arm-rpm-20.03.git) | 20.03 LTS / SP1–SP4 |
| [`third-party/rpms/openEuler-22.03/`](https://github.com/wangxumarshall/opendcdiag-arm-rpm-22.03.git) | 22.03 LTS / SP1–SP4 |
| [`third-party/rpms/openEuler-24.03/`](https://github.com/wangxumarshall/opendcdiag-arm-rpm-24.03.git) | 24.03 LTS / SP1–SP4（SP3 = 基准版本） |

```bash
git clone --recurse-submodules <repo-url>   # 含子模块
cd third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/built
./run-sdcshield.sh --list-tests            # 自动设 LD_LIBRARY_PATH=./libs
./run-sdcshield.sh -T forever -t 60s -Y -F                # 首次检测到SDC后，停止 
./run-sdcshield.sh -T forever -t 60s -Y -ignore-timeout   # 一起跑，就算检测到SDC后，也一直往后跑
./run-sdcshield.sh -T forever -t 600s -Y -e "fma*" -e "eigen_svd*" -e "eigen_gemm*" -e "zstd*" -e "zlib*" 
./run-sdcshield.sh -t 60s -n 1 -e zstd19 # 单线程，规避大核数 ULP 数值 flakiness
```

> **SP 必须与目标机一致**：SP3 的 `glibc-devel` 携带 `Requires: glibc = <sp3-N>`，装到 SP4 会触发受保护 `glibc` 降级死结。`install-deps.sh` 通过 `.os-version` 标记在安装前拦截错配。

### 从源码构建

```bash
# openEuler 24.03（基准平台）依赖；Ubuntu/Fedora 见 docs/offline-build-dependencies.md
sudo dnf install -y meson ninja-build gcc g++ cmake boost-devel zlib-devel libzstd-devel gtest-devel
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
ninja -C builddir
./builddir/sdcshield --list-tests        # 应列出 264 个 PROD 用例
```

> ARM64 要求 Eigen 5.0.0+（系统 Eigen 3.3.x 在 GCC 12+ 下编译失败）。仓库自带 `third-party/eigen5/`，aarch64 构建路径在 `tests/cpu/meson.build` 中直接 `include_directories` 指向它，**无需系统安装 eigen3**；`PKG_CONFIG_PATH` 仅为兼容 x86 路径而保留，带上无害。



## 离线与多版本构建

离线工具在 `scripts/offline-build/`。单机原生路径（`download-deps.sh`/`install-deps.sh`/`build.sh`，共享 `_common.sh`，用 `.os-version` 标记强制版本匹配）只支持 **24.03**——`_common.sh` 基线硬编码 `24.03LTS`，`detect_os_version_full` 对 22.03/20.03 会误报。22.03/20.03 必须走容器路径（D）。

```bash
cd scripts/offline-build/
# A. 下载 RPM 树（有网机，与目标机同 SP）
./download-deps.sh                     # 当前 SP 一版 → ./sdcshield-rpms/
./download-all-versions.sh             # 全 LTS/SP，对 24.03 SP3 基准取同名包集交集
./supplement-20.03-gcc10.sh all         # 20.03 专用：补 gcc-10 工具集 + meson 0.59（20.03 自带 gcc-7）
# B. 离线装依赖（目标机，无网）→ 传对应 SP 的 RPM 目录，版本会被核对
./install-deps.sh ../../third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3
# C. 目标机原生构建（依赖已装，仅 24.03）
./build.sh                              # meson + ninja + 冒烟（zstd19 -n 1）
# D. 按版本容器构建（免主机安装；源码只读挂载）—— 22.03/20.03 必走此路
./container-build.sh 24.03 SP3          # → build-out/openEuler-24.03LTS_SP3/sdcshield
./container-build.sh 22.03 SP3          # 注入 -DOPENEULER_22_03 + C++23 polyfill
./container-build.sh 20.03 SP4          # gcc-toolset-10 + 仓内 vendored meson 0.59.4
# E. 打包到对应版本 built/ 目录
./package-built-artifacts.sh 24.03 SP3
# F. 纯净容器验证（仅挂 built/，验证可直接运行）
./verify-built-pristine.sh 24.03 SP3        # 全量；eigen 类 -n 1
./verify-built-pristine.sh 24.03 SP3 smoke  # 快：--list-tests + zstd19
# G. 构建容器内跑全量用例
./run-full-tests.sh 24.03 SP3 [build]
```

> **22.03 / 20.03 适配仅在容器内进行**：旧工具链（gcc-10、meson 0.59/0.54、binutils 2.34）缺 C++20/23 特性。`container-build.sh` 只读挂载源码树，在可丢弃副本上注入 `framework/compat/cpp23_polyfill.h`、版本宏（`OPENEULER_22_03`/`OPENEULER_20_03`）和 `-D` meson option（`-Denable_acl=disabled`）——host 源码不动，x86/24.03 参考路径不受影响。历史上的 `sed` 补丁（`string::contains`→`.find()`、`udf`→`.inst`、ACL 置空、`meson_version` 放宽）**已全部收敛到源码/meson option**，容器内对源码副本零修改，只剩 CXXFLAGS/polyfill 注入。

完整依赖与排坑见 [docs/offline-build-dependencies.md](docs/offline-build-dependencies.md)。

### 多版本一键构建与部署（15 个 OS 版本）

上面 D→E→F 的单 SP 手动链，由 `build-all.sh` 自动化封装（含镜像烘焙、哈希 skip、`-P` 并行、`--since`、smoke/full 五档）。要一次性构建全 15 个版本（20.03/22.03/24.03 × LTS+SP1~SP4）并发布现场包：

```console
# 克隆（含 RPM 依赖子模块）→ 切到方案分支
git clone --recurse-submodules https://github.com/wangxumarshall/opendcdiag-arm.git
cd opendcdiag-arm && git checkout feat/multi-version-build-deploy
# 全 15 版本构建 + 纯净验证（发布闸门）→ 各 SP 产物进 built/
./scripts/offline-build/build-all.sh --all --full --jobs 3   # 期望 PASS: 15
# 产出现场 tarball（~6MB，含自动检测 OS 的 run.sh）
./scripts/offline-build/package-release.sh 24.03 SP3
# 目标机：解压后 ./run.sh -e zstd19 -t 2000 -n 1   # run.sh 自动精确匹配 OS 版本，不匹配则硬停指路
```

> `run.sh` 部署逻辑：检测本机 OS → 精确匹配 `built-index.tsv`（不跨版本回退）→ 校验 `binary-sha256` → `exec run-sdcshield.sh`（设 `LD_LIBRARY_PATH` 指向随包 `libs/`）。

完整设计与操作指南见 [docs/multi-version-build-deploy.md](docs/multi-version-build-deploy.md)（设计方案）与 [docs/multi-version-build-deploy-retrospective.md](docs/multi-version-build-deploy-retrospective.md)（一键复现指南 + 15 SP 全 full 验证基线 + 复现 gotcha）。快速开始速查见 [scripts/offline-build/README.md](scripts/offline-build/README.md)。




## 可选：启用 OpenSSL SHA（`openssl_sha`）

`openssl_sha` 经 OpenSSL 计算 SHA-256/384/512 与 golden 比对。默认不构建，由 `ssl_link_type`（默认 `none`）控制。

| `ssl_link_type` | 行为 |
|---|---|
| `none`（默认）| 不用 OpenSSL；无 `openssl_sha` |
| `dynamic` | 构建期链接 `libcrypto` |
| `static` | 同上，链接静态 `libcrypto` |
| `loaded` | 运行期 `dlopen()` 加载 `libcrypto` |

```bash
sudo dnf install -y openssl-devel
PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir --buildtype=release -Dssl_link_type=dynamic
ninja -C builddir && ./builddir/sdcshield --list-tests | grep openssl_sha
```


## 测试用例与检测能力

当前 ARM64 构建（Kunpeng 920 / openEuler 24.03 SP3）共 **273 个用例**：PROD 264、BETA 4、SKIP 5。许多用例沿用上游 x86 名字（如 `mesh_upi_avx2_*`、`ipsec_*_avx`、`fma_*_avx512`），但实现已落到 NEON / ARM 原生指令，命名保留是为与 x86 参考用例跨架构比对。

| 检测域 | 代表用例 | 检测能力 |
|---|---|---|
| 内存 / 拷贝 | `memcpy_l{1d,2,3}_cache_size`、`memcpy_rewr`、`mem_disambiguation`、`mmu_stress_arm` | 各级缓存带宽、store-to-load 转发、跨行一致性与内存序、TLB 扰动 |
| 缓存 / 互联 | `cachebounce`、`mesh_upi_*`（27） | cache line 弹跳、CLFLUSH 压力、MESH/UPI 多核读写协同 |
| 锁 / 原子 | `lock*`、`lockless_cmpxchg*`、`atomic_simd_*`、`spinlock_*`（32） | 锁指令、无锁 cmpxchg、128/256/512 位原子、自旋锁各类竞争（ARM64 atomic） |
| 向量 / SIMD | `swizzle`、`insert_extract`、`kreg1`–`kreg9`、`gather*` | NEON 排列/插入抽取、掩码寄存器（x86 k-reg 仿真）、gather/scatter |
| FMA / 浮点 | `fma`、`fma_patterns_*`、`fma_tail*`、`fpu_special_values` | FMA 模式与尾数精度穷举、特殊值逐字节 golden 比对 |
| 算术 / 大整数 | `adcx`、`adox`、`adcxlong`、`adcx_arm`、`bigint_mulx_arm`、`gmp_big*` | 进位/溢出链、GMP 大整数乘加、高汉明距离操作数压满加法器 |
| CRC / 校验 | `crc32`、`isal_crc{32,64}_*`、`zpclmul*` | `crc32` 指令、isa-l CRC32/CRC64 各标准、zlib PCLMUL 折叠 |
| 压缩 | `zlib*`、`zstd*`、`zfuzz` | zlib/zstd 压缩-解压往返、各级别、fuzz |
| 线性代数（Eigen） | `eigen_gemm_*`、`eigen_sparse`、`eigen_svd*`（含 `_cdouble_sve`） | GEMM、稀疏 Cholesky、SVD（BDCSVD/Jacobi）施压 FMA/向量 |
| IPSec / 密码 | `ipsec_*`（46） | AES-CBC/CTR/GCM、HMAC-SHA1/2、XCBC/CMAC/3DES-DOCSIS 于 NEON |
| OpenSSL SHA | `openssl_sha` | SHA-256/384/512 vs golden（需 `ssl_link_type≠none`） |
| ARM 加密扩展 | `arm_crypto` | AES（AESE/AESMC）crypto 数据通路 |
| 虚拟化 / 系统寄存器 | `vmx_vmexit_*`、`vmxmsr` | guest 触发 vmexit 退出路径一致性 |
| ARM64 SDC 专项 | `arm64_sdc`、`power_virus_dit`、`ooo_dep_chain_arm`、`lsu_store_forward_arm`、`l2c_cross_cache_line_arm`、`mmu_split_tlb_arm` | di/dt 电压骤降、乱序依赖链、LSU 转发、L2 跨行、MMU/TLB/页表遍历器 |
| IST 硬件自检 | `ist`、`ist_array`、`ist_sbaf` | ARM64 In-Silicon Test（当前 placeholder，见下表） |

### 用例质量分级

| 级别 | 名称 | 运行条件 | 实有数量 |
|---|---|---|---|
| -1 | SKIP | `quality >= -1` | 5 |
| 0 | BETA | `quality >= 0` | 4 |
| 2 | PROD（默认）| `quality >= 2` | 264 |
| | **合计** | | **273** |

- **BETA（`--quality=0`）**：`arm64_sdc`、`arm_crypto`、`ist_sbaf`、`neon_add`
- **SKIP（`--quality=-1`）**：`smi_count`、`eigen_svd_jacobi`、`eigen_svd_jacobi_cdouble`、`eigen_svd_jacobi_double`、`eigen_svd_jacobi_fvectors`

### 占位用例的诚实跳过

暂未实现的特性用例返回 `EXIT_SKIP` 并附理由（而非 `EXIT_SUCCESS` 伪装通过）。实测 `skip-reason`：

| 用例 | 跳过理由 |
|---|---|
| `ist` / `ist_array` / `ist_sbaf` | `to be implemented (placeholder): ARM64 In-Silicon Test (IST) backend not yet available; test reserved as the counterpart of Intel IFS` |
| `smi_count` | `to be implemented (placeholder): SMI counting requires a per-CPU firmware/RAS-interrupt counter not available on this architecture` |

> `mce_check` 是真实 EDAC 后端测试（统计 `/proc/interrupts` 的 EDAC `ce/ue_count`），实测 `exit: pass`，非 placeholder。

## 运行测试

>>>>>>> main
```console
./builddir/sdcshield --list-tests                        # 列 PROD 用例（默认）
./builddir/sdcshield -e zstd19 -t 5000                   # 单测试，5 秒，全核
./builddir/sdcshield -e zstd19 -t 5000 -n 1             # 单线程，规避 192 核 ULP flakiness
./builddir/sdcshield --quality=0 -e arm64_sdc -t 5000   # 跑 BETA 用例
./builddir/sdcshield --quality=-1 -e eigen_svd_jacobi   # 跑 SKIP 用例
./builddir/sdcshield -l                                  # 用例 + 描述 + 分组
./builddir/sdcshield --list-groups                       # @compression / @ipsec / @math
./builddir/sdcshield --dump-cpu-info                     # CPU + 特性 + 拓扑
./builddir/sdcshield --on-crash=context -e selftest_sigsegv -vv   # 崩溃回溯
```

Eigen SVD：`eigen_svd_cdouble` 跑在 NEON 后端；`eigen_svd_cdouble_sve` 仅 SVE 硬件运行，Kunpeng 920 在 init 阶段干净跳过。

## 架构支持

| 架构 | 状态 | 说明 |
|---|---|---|
| ARM64（AArch64） | ✅ 全支持 | Kunpeng 920、通用 ARMv8.1+ |
| x86-64 | 参考架构 | Intel 原始代码路径，移植中不动 |

ARM64 能力：CPU 特性检测（FP/NEON/CRC32/Crypto/SVE/SVE2）、拓扑检测（ACPI PPTT/sysfs/device tree）、SDC 检测（EDAC ECC、CRC32/CRC64）、SIMD（NEON 128 位 + 256/512 仿真）、RAS/ECC（EDAC/ACPI APEI）。

## 延伸

- [编写测试指南](docs/writing_tests.md) — 框架处理了测试生命周期、线程模型、CPU 特性识别、RNG 等样板代码
- [离线构建依赖与排坑](docs/offline-build-dependencies.md) — 完整依赖树、版本管制、坑点
- [贡献指南](CONTRIBUTING.md) · [行为准则](CODE_OF_CONDUCT.md)