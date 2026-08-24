# scripts/offline-build — 多 openEuler 版本构建与部署

为 openEuler 20.03 / 22.03 / 24.03 三系列 × LTS+SP1~SP4 共 **15 个**版本各构建一个原生 OpenDCDiag 二进制,打包后下载到指定环境运行。完整设计见 [`docs/multi-version-build-deploy.md`](../../docs/multi-version-build-deploy.md)。

## 快速开始(从零 100% 复现)

```bash
# 1. 克隆(含 RPM 依赖树子模块,各 1~1.7GB,需磁盘 ~4GB)
git clone --recurse-submodules https://github.com/wangxumarshall/opendcdiag-arm.git
cd opendcdiag-arm

# 2. 构建一个版本(以 24.03 SP3 基准为例)
#    build-all.sh: 镜像就绪 → 源码构建 → 打包 → 纯净验证
./scripts/offline-build/build-all.sh 24.03 SP3 --smoke

# 期望输出:
#   [1/5] 依赖已就绪(镜像烘焙) — 跳过安装
#   [273/273] Linking target opendcdiag
#   list-tests: 220
#   exit: pass
#   RESULT: PASS openEuler-24.03LTS_SP3
```

## 脚本一览

| 脚本 | 作用 |
|---|---|
| `images/build-images.sh` | 烘焙构建依赖的容器镜像层(从 quay base + RPM 树强装)。幂等:输入哈希不变则 skip |
| `container-build.sh` | 单 SP 源码构建(镜像已烘焙则跳过装包)。22.03/20.03 注入 polyfill + 版本宏 |
| `build-all.sh` | 15 矩阵编排器(5 效率杠杆:deps 烘焙/哈希 skip/--since/-P 并行/smoke|full) |
| `package-built-artifacts.sh` | 产物进 RPM submodule 的 `built/`(二进制+libs+run+BUILD-HASH+MANIFEST+VERSION) |
| `verify-built-pristine.sh` | 决定性闸门:纯净容器(只挂 built/)跑——"下载即跑" |
| `package-release.sh` | 产出现场 tarball(~6MB,含 run.sh 自动检测 OS + 精确匹配) |
| `run.sh` | (随 tarball)目标机入口:检测 OS → 精确匹配 built-index → 校验 sha256 → exec |

## 部署到目标机

```bash
# 在构建机产出某版本 tarball
./scripts/offline-build/package-release.sh 24.03 SP3
# → dist/opendcdiag-openEuler-24.03LTS_SP3-<sha8>.tar.gz

# 拷到目标机解压后直接跑(自动检测 OS,精确匹配,不匹配则硬停指路)
tar xzf opendcdiag-openEuler-24.03LTS_SP3-*.tar.gz
./run.sh -e zstd19 -t 2000 -n 1      # 或任意 opendcdiag 参数
```

## 15 个版本的构建矩阵

```
            20.03 (gcc-10 toolset)    22.03 (gcc-10)        24.03 (gcc-12)
LTS   openEuler-20.03LTS        openEuler-22.03LTS        openEuler-24.03LTS
SP1   ...LTS_SP1                 ...LTS_SP1                 ...LTS_SP1
SP2   ...LTS_SP2                 ...LTS_SP2                 ...LTS_SP2
SP3   ...LTS_SP3  ← 基准        ...LTS_SP3                 ...LTS_SP3
SP4   ...LTS_SP4                 ...LTS_SP4                 ...LTS_SP4
```

构建全 15:`./scripts/offline-build/build-all.sh --all --full`(发布闸门)。

## 效率杠杆(为何"高效")

| 杠杆 | 机制 | 收益 |
|---|---|---|
| 1 deps 烘焙 | 依赖装进镜像层,构建启动即就绪 | 消除每次构建装 ~300 RPM |
| 2 哈希 skip | BUILD-Hash 比对,源码未变则复用 built/ | 稳态下改一行只重建受影响 SP |
| 3 -P 并行 | `--jobs N` 并发起 podman | 墙上时间 ~1/N |
| 4 --since | `--since <ref>` 按 git diff 选 SP | 只重建受影响版本 |
| 5 smoke/full | smoke 秒级(开发期),full 全量(发布) | 开发快,发布稳 |

## 体积(实测)

| 对象 | 体积 | 归宿 |
|---|---|---|
| 单 SP RPM 树 | 196–346 MB | RPM submodule(git) |
| 单 SP 容器镜像 | 210–595 MB | ghcr.io Registry(不入 git) |
| 单 SP built/ 产物 | 2.8–19 MB | submodule built/(git) |
| 现场 tarball | ~6 MB | dist/(gitignored) |
| 主仓脚本 | < 25 KB | 主仓 git |
