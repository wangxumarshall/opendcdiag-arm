# OpenDCDiag-ARM
OpenDCDiag-ARM is an open-source project designed to identify defects and bugs in CPUs ported from Intel's OpenDCDiag. It consists of a set of tests built around a sophisticated CPU testing framework.

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
