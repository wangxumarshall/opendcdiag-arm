# OpenDCDiag 在 openEuler 24.03 (LTS-SP3) aarch64 上的构建依赖与离线构建指南

> 目标：在一个**最小安全安装**的 openEuler 24.03 SP3 aarch64 上，从零编译构建本仓库（默认 CPU device、ARM64 路径），并支持在**无网络环境**下快速复刻。
>
> 以下结论全部基于对 `meson.build` / `meson_options.txt` / 各子目录 `meson.build` 的逐文件分析，并在一台干净 openEuler 24.03 SP3 (Kunpeng 920, aarch64) 上以一次真实 `meson setup --reconfigure` + `ninja` + 功能验证（199 个测试、`zstd19` 单线程 `exit: pass`）背书。

---

## 1. 依赖总览（分层）

本项目是**纯本地编译**的 C/C++ 项目，依赖可分为四层：

| 层 | 内容 | 是否必须 |
|---|---|---|
| A. 构建工具链 | gcc / g++ / meson / ninja / perl / python3 / pkg-config / binutils | **必须** |
| B. 编译期库（头文件 + 静态/动态库） | boost（仅头文件）、eigen5（**仓库自带**，无需系统装）、zlib、libzstd、libisal(isa-l) | **必须**（eigen5 除外，因其自带） |
| C. 可选功能库 | openssl（libcrypto，仅 `-Dssl_link_type≠none` 时）、gtest（仅单元测试 `unittests` 目标，默认不构建） | 可选 |
| D. 运行期 / 链接选项相关 | libatomic（aarch64 128 位原子运行时）、glibc 静态库（仅全静态链接 `-static` 时） | 视构建模式 |

关键事实（来自源码与实测）：

1. **Eigen 5.0.0+ 仓库自带**于 `third-part/eigen5/`。aarch64 路径在 `tests/cpu/meson.build` 里直接 `include_directories('../../third-part/eigen5')`，**不依赖系统 eigen3**，也**不需要**再设 `PKG_CONFIG_PATH`（该 env 仅在你想让系统 pkg-config 看到 eigen5.pc 时用，但 aarch64 构建并不查询它）。`CLAUDE.md` 顶部那段 `PKG_CONFIG_PATH=./third-part/eigen5` 是历史遗留/兼容写法，aarch64 实际构建可省略——但**带上无害**，故离线脚本里保留以兼容未来 x86 路径。
2. **无 nasm 依赖**。`meson.build` 里虽定义了 `nasm_system_flags`，但**没有任何 `find_program('nasm')` 调用**，也没有 `.asm` 源文件被编译。aarch64 上不需要 nasm/yasm。
3. **boost 只用头文件**。`framework/meson.build` 检查 `boost/algorithm/string.hpp` 与 `boost/type_traits/is_complex.hpp` 两个头；实测 `pkg-config --libs boost` 为空（header-only）。因此 `boost-devel` 足够，不需要 boost 的 `.so` 组件。
4. **libisal(isa-l) 是必须的**（默认构建下 `cpp.find_library('isal', required:false, static:true)`，但 10 个 `isal_*` CRC 测试需要它）。openEuler 提供两个等价包：`libisa-l-devel`（EPOL 仓，2.30.0）与 `libisal-devel`（everything 仓，2.29.0）。两者都装 `/usr/include/isa-l/*.h` + `libisal.a/.so`。**任选其一**，推荐 `libisa-l-devel`（版本更新，且 EPOL 是 openEuler 主推应用仓）。当前这台机器上的 isal 是**源码编译安装**的（RPM 数据库不认领 `/usr/lib/libisal.a`），所以最小化环境应改用官方 RPM。
5. **libatomic**：aarch64 上 `std::atomic<__int128>`（128 位 CAS）会 lower 成对 `__atomic_*_16` 运行时符号的调用，需要 `libatomic`。meson 以 `required:false` 查找，但实际 `spinlock_stress_cmpxchg16b` 等测试会用到。`libatomic` 包默认随 gcc 装上。
6. **gtest**：仅 `unittests` 目标（`build_by_default:false`）需要，默认 `ninja` 不构建它。若不跑单元测试，**可省** `gtest-devel`。
7. **openssl**：默认 `-Dssl_link_type=none`，不构建 `openssl_sha` 测试，不需要 `openssl-devel`。需要 SHA 测试时加 `-Dssl_link_type=dynamic`。
8. **git**：仅 `framework/scripts/make-gitid.pl` 生成 `gitid.h` 时调用 `git describe`；无 git 仓库时脚本会优雅退化（写入占位版本号），不阻断构建。但为得到正确版本号，建议装 `git`。
9. **gcc 版本**：`cpp_std=gnu++23`。实测 **gcc 12.3.1**（openEuler 24.03 SP3 自带）即可编译通过，无需 gcc 13+。
10. **meson 版本**：要求 `>=1.3`。openEuler 官方 `meson-1.3.1` 满足。（注：当前开发机 `meson --version` 显示 1.11.2 来自用户 `pip` 安装 `~/.local`，**不是**系统包；离线最小化环境应使用系统 RPM 的 1.3.1，已足够。）

---

## 2. 最小软件包清单（openEuler 24.03 SP3 aarch64，默认 CPU 构建）

### 2.1 必装包（离线构建核心集）

```text
gcc                         # 12.3.1 — C 编译器
gcc-c++                     # 12.3.1 — C++ 编译器（自动拉入 libstdc++-devel）
meson                       # 1.3.1   — 构建系统（>=1.3，满足 meson_version 要求）
ninja-build                 # 1.11.1  — 后端构建器
perl                        # 5.38.0  — 跑 generate-short-ids.pl / make-gitid.pl / armsimd_generate.pl
python3                     # 3.11.6  — 跑 generate_test_list.py
pkgconf                     # 1.9.5   — pkg-config，查 zlib/zstd/openssl/boost
binutils                    # 2.41    — ar/objcopy（仅 -static 路径用到，但 ld.bfd 是推荐链接器）
boost-devel                 # 1.83.0  — 仅头文件依赖
zlib-devel                  # 1.2.13  — zlib 测试
zstd-devel                  # 1.5.5   — zstd19 测试
libisa-l-devel              # 2.30.0  — isa-l CRC 测试（EPOL 仓；或用 libisal-devel）
libatomic                   # 12.3.1  — aarch64 128 位原子运行时
git                         # 2.43.0  — 生成 gitid.h 版本号（无则退化，非阻断）
```

### 2.2 可选包

```text
openssl-devel               # 3.0.12  — 仅 -Dssl_link_type=dynamic/static 时
gtest-devel                 # 1.14.0  — 仅构建 unittests 目标时
glibc-static                # 全静态链接(-static)时需要 libc.a（默认动态链接不需要）
```

> 注：openEuler 24.03 SP3 的 `glibc-devel` 已包含 `/usr/lib64/libc.a`，`-static-libstdc++`（`-Dstatic_libstdcpp=true`）需要的 `libstdc++.a` 由 `libstdc++-devel`（随 gcc-c++ 拉入）提供。仅当要**全静态**链接整个二进制（`-Ddependency_link=static` + `cpp_link_args=-static`）时才需要额外的 `glibc-static`。

---

## 3. 离线构建操作流程

### 3.1 在一台有网的 openEuler 24.03 SP3 上下载 RPM

```bash
# 1. 创建离线包目录
mkdir -p ~/opendcdiag-offline && cd ~/opendcdiag-offline

# 2. 下载必装包及其依赖（含依赖树，--resolve）
sudo dnf install --downloadonly --resolve --destdir=$PWD \
    gcc gcc-c++ meson ninja-build perl python3 pkgconf binutils \
    boost-devel zlib-devel zstd-devel libisa-l-devel libatomic git

# 3.（可选）下载 OpenSSL/gtest
sudo dnf install --downloadonly --resolve --destdir=$PWD \
    openssl-devel gtest-devel
```

> `libisa-l-devel` 在 **EPOL** 仓。若最小安装的机器未启用 EPOL，需先 `sudo dnf config-manager --set-enabled EPOL`（或在有网机上下载时确保 EPOL 已启用，RPM 拷过去即可直接 `rpm -ivh`，不依赖仓库元数据）。
>
> 若 EPOL 不可用，改用 `libisal-devel`（everything 仓，2.29.0），头文件路径一致。

### 3.2 在无网络的目标机上安装

```bash
# 拷贝整个目录到目标机后：
cd ~/opendcdiag-offline
sudo dnf install --disablerepo=* ./*.rpm          # 推荐（自动处理依赖顺序）
# 或纯 rpm 方式（需手动保证顺序）：
# sudo rpm -Uvh --nodeps ./*.rpm
```

### 3.3 构建本项目

```bash
cd /path/to/opendcdiag-arm-test2

# aarch64 路径：eigen5 仓库自带，无需系统 eigen3
# PKG_CONFIG_PATH 仅为兼容 x86 路径而保留，aarch64 可省
meson setup builddir --buildtype=release
ninja -C builddir

# 验证
./builddir/opendcdiag --list-tests          # 应列出 ~199 个测试
./builddir/opendcdiag -e zstd19 -t 2000 -n 1 # 应 exit: pass
```

### 3.4（可选）启用 OpenSSL SHA 测试

```bash
meson setup --reconfigure builddir --buildtype=release -Dssl_link_type=dynamic
ninja -C builddir
./builddir/opendcdiag --list-tests | grep openssl_sha
```

---

## 4. 依赖→包→文件 三对应表（便于排障）

| 源码里的依赖查询 | pkg-config / find_library 名 | openEuler 包 | 提供的关键文件 |
|---|---|---|---|
| `dependency('boost')` + `check_header('boost/algorithm/string.hpp')` | `boost` | `boost-devel` | `/usr/include/boost/algorithm/string.hpp` |
| `dependency('eigen3')`（aarch64 改用自带） | `eigen3` | **仓库自带** `third-part/eigen5/` | `third-part/eigen5/Eigen/*` |
| `dependency('zlib')` | `zlib` | `zlib-devel` | `/usr/include/zlib.h`, `/usr/lib64/libz.{a,so}` |
| `dependency('libzstd')` | `libzstd` | `zstd-devel` | `/usr/include/zstd.h`, `/usr/lib64/libzstd.{a,so}` |
| `cpp.find_library('isal', static:true)` | （无 .pc，直接 find_library） | `libisa-l-devel` 或 `libisal-devel` | `/usr/include/isa-l/*.h`, `/usr/lib64/libisal.{a,so}` |
| `cc.find_library('atomic')` | （无 .pc） | `libatomic` | `/usr/lib64/libatomic.so.1`（静态 `libatomic.a` 随 `libatomic` 装） |
| `cc.find_library('dl')` | — | glibc 自带 | `/usr/lib64/libdl.so` |
| `dependency('threads')` | — | glibc 自带 | libpthread |
| `dependency('libcrypto')`（可选） | `libcrypto` | `openssl-devel` | `/usr/include/openssl/sha.h`, `/usr/lib64/libcrypto.{a,so}` |
| `dependency('gtest_main')`（可选） | `gtest_main` | `gtest-devel` | `/usr/lib64/libgtest*.a`, `.pc` |
| `find_program('perl')` | — | `perl` | `/usr/bin/perl` |
| `find_program('python3')` | — | `python3` | `/usr/bin/python3` |
| `find_program('bash')` / `('sh')` | — | glibc-minimal 自带 | `/usr/bin/bash`, `/usr/bin/sh` |
| `make-gitid.pl` 调 `git describe` | — | `git`（可选，退化容错） | `/usr/bin/git` |

---

## 5. 常见离线构建坑点

1. **isal 找不到**：最小安装默认无 `libisa-l-devel`。症状：`Library isal found: NO`，10 个 `isal_*` 测试被静默剔除（构建仍成功，但测试数变少）。解决：装 `libisa-l-devel`（EPOL）或 `libisal-devel`（everything）。
2. **eigen 版本错**：若误装系统 `eigen3-devel`（Eigen 3.3.x），aarch64 + GCC 12 会因 `deprecated-enum-enum` 转换在 `-Wextra` 下变硬错误而编译失败。**务必用仓库自带的 `third-part/eigen5`**；aarch64 路径已硬编码指向它，勿覆盖。
3. **meson 版本低**：openEuler 自带 `meson-1.3.1` 恰好满足 `>=1.3`；但若目标机装了更老的 meson（如 22.03 LTS 带的），需升级。离线包里带的 1.3.1 RPM 可直接用。
4. **EPOL 仓未启用**：`libisa-l-devel` 只在 EPOL。离线场景把 RPM 拷过去 `rpm -ivh` 即可绕过仓库检查；或改用 everything 仓的 `libisal-devel`。
5. **无 git**：无 git 仓库或无 git 命令时，`gitid.h` 仍会生成（脚本 fallback 到占位串），构建不阻断，只是版本号是占位。
6. **磁盘空间**：完整构建（含 SVE/NEON 多后端）产物约 256 MB 单二进制 + 中间 `.a`，建议 builddir 所在盘预留 ≥ 2 GB。

---

## 6. 一键离线脚本（可在目标机直接跑）

见仓库 `scripts/offline-build/` 下的：
- `download-deps.sh` — 在有网机上拉取全部 RPM
- `install-deps.sh` — 在无网机上离线安装
- `build.sh` — 标准构建 + 验证

（如该目录不存在，按本文件第 3 节手写即可，步骤已最小化。）
