# OpenDCDiag-ARM
OpenDCDiag-ARM is an open-source project designed to identify defects and bugs in CPUs ported from Intel's OpenDCDiag. It consists of a set of tests built around a sophisticated CPU testing framework. 
## Building OpenDCDiag-ARM
### Prerequisites
#### openEuler
OpenDCDiag-ARM has been built and tested on openEuler 24.03 (LTS-SP3).
```console
# Install dependencies (requires root)
sudo dnf install -y meson ninja-build gcc g++ cmake boost-devel zlib-devel libzstd-devel gtest-devel
# Note: Eigen 5.0.0 is required (see Building section)
```
#### Ubuntu
OpenDCDiag-ARM has been built and tested on Ubuntu 21.04 and 21.10.
Before building, the following prerequisites must be installed.
```console
sudo apt-get install gcc g++ cmake libeigen3-dev libboost-all-dev libzstd-dev zlib1g-dev libgtest-dev meson
```
#### Fedora
OpenDCDiag-ARM has been built and tested on Fedora 33 and 34.
Before building, the following prerequisites must be installed.
```console
sudo dnf install -y boost-devel eigen3-devel gcc gcc-c++ git gtest-devel meson zlib-devel libzstd-devel
```
### Architecture Support
OpenDCDiag-ARM supports the following architectures:
| Architecture | Status | Notes |
|--------------|--------|-------|
| ARM64 (AArch64) | ✅ Full Support | Kunpeng 920, generic ARMv8.1+ |
#### ARM64 (AArch64) Support
OpenDCDiag-ARM provides full support for ARM64 architecture, including:
- **CPU Detection**: FP, NEON, CRC32, Crypto extensions, SVE/SVE2
- **Topology Detection**: ACPI PPTT, sysfs, device tree
- **SDC Detection**: ECC errors, CRC32/CRC64 validation
- **SIMD Operations**: NEON (128-bit) with 256/512-bit emulation
- **RAS/ECC**: EDAC subsystem, ACPI APEI, vendor drivers
**Building for ARM64 (Native)**:
```console
# ARM64 requires Eigen 5.0.0+ (system Eigen 3.3.x is incompatible with GCC 12+)
# Prepare Eigen 5.0.0 if not available in system
tar -xjf eigen-5.0.0.tar.bz2
# Create pkg-config file for custom Eigen
cat > eigen-5.0.0/eigen3.pc << 'EOF'
prefix=/path/to/eigen-5.0.0
exec_prefix=${prefix}
Name: Eigen3
Description: A C++ template library for linear algebra
Version: 5.0.0
Cflags: -I${prefix}
EOF
# Configure with custom Eigen
PKG_CONFIG_PATH=./eigen-5.0.0 meson setup builddir --buildtype=release
# Build
ninja -C builddir
```
**Kunpeng 920 Optimized Build**:
```console
meson builddir --buildtype=release -Dkunpeng_optimize=true
ninja -C builddir
```
**Building with OpenSSL SHA (openssl_sha test)**:
The `openssl_sha` test computes SHA-256/384/512 checksums via OpenSSL and
compares them against golden values to detect silent data corruption (SDC).
It is **not built by default** — OpenSSL is an optional dependency, gated by
the `ssl_link_type` meson option (default `none`).

Install the OpenSSL development package, then configure with
`-Dssl_link_type=<mode>`:

| `ssl_link_type` | Behavior |
|-----------------|----------|
| `none` (default)| OpenSSL not used; `openssl_sha` is absent from the binary |
| `dynamic`       | Links `libcrypto` at build time; test calls it directly |
| `static`        | Same as `dynamic`, but links the static `libcrypto` |
| `loaded`        | `dlopen()`s `libcrypto` at runtime (no link-time dependency) |

```console
# openEuler: install OpenSSL development headers (requires root)
sudo dnf install -y openssl-devel
# Ubuntu: sudo apt-get install libssl-dev
# Fedora: sudo dnf install -y openssl-devel

# Configure with OpenSSL SHA enabled (dynamic linking), using Eigen 5
PKG_CONFIG_PATH=./eigen-5.0.0 meson setup builddir --buildtype=release \
    -Dssl_link_type=dynamic
# Build
ninja -C builddir
# Confirm openssl_sha is now in the test catalog
./builddir/opendcdiag --list-tests | grep openssl_sha
# Run the OpenSSL SHA test
./builddir/opendcdiag -e openssl_sha -t 5000
```
If the configure step cannot find `libcrypto`, verify that
`pkg-config --modversion libcrypto` prints a version (provided by the
`openssl-devel` / `libssl-dev` package above).

> Note: on a build that was configured without `-Dssl_link_type`, you must
> reconfigure (`meson setup --reconfigure builddir -Dssl_link_type=dynamic`)
> or create a fresh build directory — `ninja` alone will not pick it up.

**Running ARM64 Tests**:
ARM64-specific tests (arm64_sdc, neon_add, arm_crypto, etc.) are quality level BETA.
Use `--quality=0` to enable them:
```console
# List all tests including ARM64 beta tests
./builddir/opendcdiag --quality=0 --list-tests
# Run ARM64 SDC test
./builddir/opendcdiag --quality=0 -e arm64_sdc -t 5000
# Run NEON test
./builddir/opendcdiag --quality=0 -e neon_add -t 5000
```
**Eigen SVD on ARM64 (NEON + SVE)**:
The `eigen_svd_cdouble` test (complex-double BDCSVD, used to stress FMA/vector
units for SDC detection) is the ARM64-native counterpart of the x86 AVX-512
build. On ARM64 it runs on the NEON backend by default; on x86-64 it keeps the
original AVX-512 gating (`minimum_cpu = cpu_skylake_avx512`).

```console
# eigen_svd_cdouble runs on the NEON backend (default ARM64 build)
./builddir/opendcdiag --quality=-1 -e eigen_svd_cdouble -t 5000
```

For SVE-capable hardware (e.g. Kunpeng 930), a second variant,
`eigen_svd_cdouble_sve`, is compiled against Eigen's SVE packet backend and
built automatically on aarch64. On a CPU without SVE (e.g. Kunpeng 920) it
reports a clean `CpuNotSupported` skip at init time and never executes any SVE
instructions.
```console
# Runs only on SVE hardware; skips cleanly on Kunpeng 920
./builddir/opendcdiag --quality=-1 -e eigen_svd_cdouble_sve -t 5000
```
For detailed ARM64 build information, see [docs/BUILD_ARM64.md](docs/BUILD_ARM64.md).

### Multi-Version Build & Deploy (多 openEuler 版本构建部署)

单机原生构建见上;若需在 **openEuler 20.03 / 22.03 / 24.03 三系列 × LTS+SP1~SP4(共 15 个版本)** 上各产出原生二进制并部署(受版本间 gcc-7/10/12 与依赖库差异限制,需逐版本构建),用 `scripts/offline-build/` 下的离线构建链:容器镜像烘焙依赖(ghcr.io)、15 矩阵编排(5 效率杠杆:deps 烘焙/哈希 skip/-P 并行/--since/smoke|full)、逐版本纯净验证后打包发布 tarball。

```console
# 克隆(含 RPM 依赖子模块)
git clone --recurse-submodules https://github.com/wangxumarshall/opendcdiag-arm.git
cd opendcdiag-arm && git checkout feat/multi-version-build-deploy
# 一键构建验证全 15 版本(发布闸门)
./scripts/offline-build/build-all.sh --all --full --jobs 3   # 期望 PASS: 15
# 产出现场 tarball(~6MB,含自动检测 OS 的 run.sh)
./scripts/offline-build/package-release.sh 24.03 SP3
# 目标机:解压后 ./run.sh -e zstd19 -t 2000 -n 1   # 自动精确匹配 OS 版本
```

完整设计与操作指南见:
- [docs/multi-version-build-deploy.md](docs/multi-version-build-deploy.md) — 完整设计方案(三层存储分层、镜像构建子系统、build-all 编排、适配层收敛、CI 矩阵、落地顺序)
- [docs/multi-version-build-deploy-retrospective.md](docs/multi-version-build-deploy-retrospective.md) — 工作回顾与一键复现指南(最小命令序列、15 SP 全 full 验证基线、复现 gotcha、patches 历程)

快速开始速查见 [scripts/offline-build/README.md](scripts/offline-build/README.md)。

## Test Quality Levels
OpenDCDiag-ARM tests are classified by quality levels that determine when they run:
| Level | Name | Description | Run Condition |
|-------|------|-------------|---------------|
| -1 | SKIP | Skip level | `quality >= -1` |
| 0 | BETA | Beta test | `quality >= 0` |
| 1 | (none) | Internal use | `quality >= 1` |
| 2 | PROD | Production ready (default) | `quality >= 2` |
**Note**: Default quality is 2 (PROD). Tests with level < 2 need explicit quality parameter.
### Quality=1 Tests (19 tests)
| Test | Category | Description |
|------|----------|-------------|
| eigen_gemm_double14 | Eigen GEMM | Double precision matrix multiply (14x14) |
| eigen_gemm_cdouble_dynamic_square | Eigen GEMM | Complex double dynamic matrix multiply |
| eigen_gemm_double_dynamic_square | Eigen GEMM | Double dynamic matrix multiply |
| eigen_gemm_float_dynamic_square | Eigen GEMM | Float dynamic matrix multiply |
| eigen_sparse | Eigen Sparse | Sparse matrix operations |
| eigen_svd | Eigen SVD | Singular value decomposition |
| eigen_svd_cdouble_noavx512 | Eigen SVD | Complex double SVD (no AVX512) |
| eigen_svd_double2 | Eigen SVD | Double precision SVD (variant 2) |
| eigen_svd_double | Eigen SVD | Double precision SVD |
| eigen_svd_fvectors | Eigen SVD | Float vectors SVD |
| zstd19 | Compression | Zstd compression (v1.9) |
| zstd | Compression | Zstd compression (latest) |
| zstd1 | Compression | Zstd compression (v1.0) |
| zstd_aaa | Compression | Zstd compression AAA mode |
| zfuzz | Compression | Zstd fuzz testing |
| zlib9 | Compression | Zlib compression (v1.9) |
| zlib | Compression | Zlib compression (latest) |
| zlib1 | Compression | Zlib compression (v1.0) |
| zlib_aaa | Compression | Zlib compression AAA mode |
### Quality=0 New Tests (5 tests, ARM64-specific)
| Test | Category | Description |
|------|----------|-------------|
| neon_add | NEON SIMD | NEON vector addition operations |
| neon_perf | NEON SIMD | NEON performance benchmark |
| arm_crypto | Crypto | ARM cryptographic instructions (AES/SHA) |
| arm64_sdc | SDC Detection | Silent Data Corruption detection via CRC32/CRC64 |
| arm64_stress | Stress | ARM64 stress testing |
### Quality=-1 New Tests (4 tests, Jacobi SVD variants)
| Test | Category | Description |
|------|----------|-------------|
| eigen_svd_jacobi | Eigen SVD | Generic Jacobi SVD |
| eigen_svd_jacobi_cdouble | Eigen SVD | Jacobi SVD for complex double |
| eigen_svd_jacobi_double | Eigen SVD | Jacobi SVD for double precision |
| eigen_svd_jacobi_fvectors | Eigen SVD | Jacobi SVD for float vectors |
### Running Tests by Quality
```console
# Run only PROD tests (default, quality=2)
./builddir/opendcdiag --list-tests
# Run BETA + PROD tests (quality=0)
./builddir/opendcdiag --quality=0 --list-tests
# Run all tests including SKIP (quality=-1)
./builddir/opendcdiag --quality=-1 --list-tests
```
## Contributions
The OpenDCDiag-ARM project welcomes contributions and pull requests.
Please see [Contributing to OpenDCDiag](CONTRIBUTING.md) for more
details.
## Code of Conduct
The OpenDCDiag-ARM project has adopted the Contributor's Covenant as its [Code of
Conduct][coc]. The project requires contributors and users to follow our Code
of Conduct, both in letter and in spirit.
[coc]: CODE_OF_CONDUCT.md
## Writing Tests
The OpenDCDiag-ARM framework is designed to make the creation of new CPU
tests as simple as possible. It takes care of much of the boiler
plate code CPU tests need, e.g., test life cycle, threading model, CPU
feature identification, random number generation, etc. This allows test
authors to concentrate on the specific test functionality that
interests them. A detailed guide to writing new OpenDCDiag tests is
presented in [A Guide to Writing OpenDCDiag
tests](docs/writing_tests.md).
