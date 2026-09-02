# 15 版本全量构建验证 + RPM 目录迁移实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 对全部 15 个 openEuler 版本(20.03/22.03/24.03 × LTS/SP1/SP2/SP3/SP4)完成容器镜像烘焙、sdcshield 全量构建与纯净容器端到端验证(与已完成的 3 个 LTS 相同标准);同时把每个 SP 目录下松散的 RPM 包迁移到 `rpms/` 子目录并同步适配所有脚本。

**Architecture:** 两阶段:第一阶段先做 RPM 目录结构迁移(纯移动 + 脚本适配,独立可验证,不影响后续构建逻辑);第二阶段用现有 build-all.sh 编排器跑 12 个缺失 SP 的"烘焙→构建→打包→full 验证"流水线(base 镜像经 docker hub 加速器拉取再导入 podman,绕过 quay 直连挂起)。

**Tech Stack:** podman 4.9.4 + docker 18.09(镜像搬运)+ meson/ninja 容器内构建 + git submodule。

## Global Constraints

- CLAUDE.md 补丁纪律:一个单元一个提交,提交前 100% 实测验证,不伪造结果。
- x86-64 不动规则:本次只动 scripts/offline-build/ 与 RPM submodule,不触碰 framework/tests。
- 构建验证标准(与已完成 3 个 LTS 一致):261/261 编译、list-tests 208、`verify-built-pristine.sh full` 全部 `RESULT: PASS`(eigen -n1 fail=0、全量 -n8 fail=0、crashes=0)。
- RPM 迁移目标结构:`openEuler-XX.03/openEuler-XX.03LTS[_SPx]/rpms/*.rpm`(原目录只留 `built/`、`rpms/`、`.os-version`、README 等)。
- 网络现实:quay.io 直连挂起;base 镜像一律 `docker pull`(hub 加速器)→ `docker save` → `podman load` → `podman tag` 为 quay.io 命名。
- sudo 密码 `SDC@2026`(本机 root 操作用,不写入任何文件)。

## 前置事实(已实测确认)

- 15 个 SP 目录结构统一:松散 RPM(324~437 个)+ `built/`,无其他杂项。
- `scripts/offline-build/` 中引用"SP 目录下松散 RPM"的位置(迁移必须同步改):
  1. `container-build.sh:49` — `ls "$RPMDIR_HOST"/*.rpm` 存在性检查
  2. `container-build.sh:242` — `-v "$RPMDIR_HOST:/rpms:ro,Z"` 挂载(容器内 `/rpms/*.rpm`、inner 脚本 `ls /rpms/findutils-*.rpm` 等)
  3. `images/build-images.sh:62` — `RPM_DIR=.../openEuler-${series}/${OS_TAG}`(build context,COPY *.rpm)
  4. `images/build-images.sh:137,144` — `ls "$RPM_DIR"/*.rpm`
  5. `package-built-artifacts.sh:33-34,58,85,114,143-144` — RPMDIR 与 `ls .../libatomic-*.rpm`
  6. `build-all.sh:117` — rpmdir(哈希输入 + builtdir 派生)
  7. `run-full-tests.sh:61-62` — 22.03 libatomic 自愈提取
  8. `download-all-versions.sh:14,93,101,111` — 下载产物目录(将来重新下载会写 rpms/)
  9. `download-deps.sh:90`、`install-deps.sh:61,113` — 单版本下载/安装(操作 OUTDIR,需跟随)
  10. `supplement-20.03-gcc10.sh` — 20.03 补包(下载进 SP 目录)
  11. `images/Containerfile.template:31,43` — `COPY *.rpm` / `find /rpms -maxdepth 1`(容器内路径不变,context 变)
- docker hub 有全部所需 tag(openeuler/openeuler:24.03-lts-sp3 已实测拉取成功;24.03-lts、22.03-lts、20.03-lts、20.03-lts-sp4 已在本地)。
- build-all.sh 支持 `--full`(全量验证)与哈希 skip(已构建且输入未变的 SP 自动复用,只重跑 full 验证)。

---

### Task 1: RPM 子模块内迁移 15 个 SP 目录的 RPM 到 rpms/(纯 git mv)

**Files:**
- Modify(3 个 submodule,各 15 个目录):`third-party/rpms/openEuler-{20.03,22.03,24.03}/openEuler-*LTS*/`
- 每个 submodule 产出一个迁移提交。

**Interfaces:**
- Produces: `openEuler-XX.03LTS[_SPx]/rpms/` 目录(所有 *.rpm 在内);`built/`、`.os-version` 原地不动。
- 后续 Task 2 的脚本适配将引用 `$SP_DIR/rpms`。

- [x] **Step 1: 逐 submodule 执行 git mv(保留 git 历史重命名检测)**

```bash
cd /home/sdc/wangxu/opendcdiag-arm
for s in 20.03 22.03 24.03; do
  d=third-party/rpms/openEuler-$s
  for spdir in $d/openEuler-${s}LTS $d/openEuler-${s}LTS_SP1 $d/openEuler-${s}LTS_SP2 $d/openEuler-${s}LTS_SP3 $d/openEuler-${s}LTS_SP4; do
    mkdir -p "$spdir/rpms"
    (cd "$spdir" && git mv -k *.rpm rpms/ 2>/dev/null || { for f in *.rpm; do git mv "$f" "rpms/$f"; done })
  done
  # 核验:SP 根目录零残留 rpm,数量守恒
  for spdir in $d/openEuler-${s}LTS*/; do
    n_root=$(ls $spdir/*.rpm 2>/dev/null | wc -l)
    n_rpms=$(ls $spdir/rpms/*.rpm 2>/dev/null | wc -l)
    echo "$(basename $spdir): root=$n_root rpms=$n_rpms"
  done
done
```

预期:每目录 root=0,rpms=324~437(与迁移前 root 数一致)。

- [x] **Step 2: 三个 submodule 分别提交**

```bash
for s in 20.03 22.03 24.03; do
  d=third-party/rpms/openEuler-$s
  git -C $d add -A
  git -C $d commit -m "refactor: move loose RPMs into rpms/ subdirectory

All *.rpm move from openEuler-XX.03LTS[_SPx]/ to .../rpms/ so the SP
directory holds only built/, rpms/, .os-version. Pure git mv (history-
preserving); no content change."
done
```

(暂不 push,等 Task 2 脚本适配 + Task 3 全 15 验证通过后一起推,保证远端 main 任何时刻都可用。)

### Task 2: 适配 scripts/offline-build 全部 RPM 路径引用

**Files:**
- Modify: `scripts/offline-build/container-build.sh`
- Modify: `scripts/offline-build/images/build-images.sh`
- Modify: `scripts/offline-build/package-built-artifacts.sh`
- Modify: `scripts/offline-build/build-all.sh`
- Modify: `scripts/offline-build/run-full-tests.sh`
- Modify: `scripts/offline-build/download-all-versions.sh`
- Modify: `scripts/offline-build/download-deps.sh`
- Modify: `scripts/offline-build/install-deps.sh`
- Modify: `scripts/offline-build/supplement-20.03-gcc10.sh`
- Test: 用已构建好的 24.03 LTS 做一次全链路回归(镜像 skip 判定 + container-build + package + pristine full)。

**Interfaces:**
- Consumes: Task 1 的 `.../openEuler-XX.03LTS[_SPx]/rpms/` 结构。
- Produces: 所有脚本统一从 `$SP_DIR/rpms` 取 RPM;容器内 `/rpms` 挂载点与 Containerfile 不变(挂载源改为 rpms/ 子目录)。

- [x] **Step 1: 逐脚本修改 RPM 路径**

各文件的具体改动(共 11 处,全部是"SP 目录 → SP 目录/rpms"):

1. `container-build.sh:49`: `ls "$RPMDIR_HOST"/*.rpm` → `ls "$RPMDIR_HOST/rpms"/*.rpm`
2. `container-build.sh:242`: 挂载 `-v "$RPMDIR_HOST:/rpms:ro,Z"` → `-v "$RPMDIR_HOST/rpms:/rpms:ro,Z"`(inner 脚本零改动,容器内仍是 /rpms)
3. `build-images.sh:62`: `RPM_DIR=".../${OS_TAG}"` → `RPM_DIR=".../${OS_TAG}/rpms"`
4. `build-images.sh:137,144`: 引用自动跟随 RPM_DIR,无需另改(用 grep 确认)
5. `package-built-artifacts.sh:33`: `RPMDIR=.../${OS_TAG}` → `.../${OS_TAG}/rpms`(58/85/114 行挂载与 /rpms 引用自动跟随;144 行 LTS libatomic 取 `.../openEuler-${SERIES}LTS/rpms`)
6. `build-all.sh:117`: rpmdir 哈希输入加 `/rpms`(builtdir 仍用 SP 根: `$rpmdir/built` → 需拆成两个变量,`rpmdir` 用于哈希、`builtdir=SP 根/built`)
7. `run-full-tests.sh:61-62`: `RPMDIR=.../${OS_TAG}` → `.../${OS_TAG}/rpms`
8. `download-all-versions.sh:14,93,101,111`: 下载产物写入 `.../rpms/`(OUTDIR 拼接处)
9. `download-deps.sh:90`、`install-deps.sh:61,113`: OUTDIR 加 `/rpms`(install-deps.sh 操作的是"传入目录",改为调用方传 rpms/ 或脚本内 `cd rpms/`,以实际代码为准)
10. `supplement-20.03-gcc10.sh`: 下载落点加 `/rpms`

- [x] **Step 2: 回归验证已构建的 24.03 LTS 全链路**

```bash
cd /home/sdc/wangxu/opendcdiag-arm
# 镜像 skip 判定仍命中(manifest + 本地镜像)
./scripts/offline-build/images/build-images.sh 24.03 LTS 2>&1 | grep -E 'skip|自检'
# 强制重跑构建 + 打包 + full 验证(证明新路径下全链路可用)
./scripts/offline-build/container-build.sh 24.03 LTS 2>&1 | tail -5
./scripts/offline-build/package-built-artifacts.sh 24.03 LTS 2>&1 | tail -3
./scripts/offline-build/verify-built-pristine.sh 24.03 LTS full 2>&1 | tail -3
```

预期:build-images 报 skip;container-build 261/261 + list-tests 208 + zstd19 pass;pristine `RESULT: PASS`。

- [x] **Step 3: 提交(主仓)**

```bash
git add scripts/offline-build/
git commit -m "refactor(offline-build): follow RPMs into rpms/ subdirectory

All RPM path references updated from <SP-dir>/*.rpm to <SP-dir>/rpms/*.rpm
(10 files, container mounts now bind rpms/ to /rpms — in-container paths
unchanged). Verified end-to-end on 24.03 LTS: image skip hit, 261/261
compile, list-tests 208, pristine full RESULT: PASS."
```

### Task 3: 获取 12 个缺失 base 镜像(docker→podman 桥接)

**Files:** 无代码改动(纯环境操作)。

- [x] **Step 1: docker 拉取 12 个 tag 并导入 rootless podman**

缺失清单:24.03-lts-sp{1,2,4}(sp3 已拉)、22.03-lts-sp{1,2,3,4}、20.03-lts-sp{1,2,3}(sp4 已拉)。

```bash
# 批量拉取(docker hub 走已配置的加速器)
for tag in 24.03-lts-sp1 24.03-lts-sp2 24.03-lts-sp4 22.03-lts-sp1 22.03-lts-sp2 22.03-lts-sp3 22.03-lts-sp4 20.03-lts-sp1 20.03-lts-sp2 20.03-lts-sp3; do
  echo 'SDC@2026' | sudo -S docker pull openeuler/openeuler:$tag
done
# 逐个搬运到 rootless podman 并打 quay.io tag
for tag in <上表>; do
  echo 'SDC@2026' | sudo -S bash -c "docker save openeuler/openeuler:$tag -o /home/sdc/.claude-tmp/oe-$tag.tar && chmod 644 /home/sdc/.claude-tmp/oe-$tag.tar && chown sdc:sdc /home/sdc/.claude-tmp/oe-$tag.tar"
  podman load -i /home/sdc/.claude-tmp/oe-$tag.tar
  podman tag localhost/openeuler/openeuler:$tag quay.io/openeuler/openeuler:$tag
  rm /home/sdc/.claude-tmp/oe-$tag.tar
done
```

注意:docker hub 无 `20.03-lts-sp2/sp3` 之类 tag 时实测确认(20.03-lts-sp4 已存在;若个别 tag 缺失,回退 quay.io API 确认存在性后用 `skopeo copy docker://docker.1ms.run/... dir:` 或 docker 直拉 quay 镜像源——docker 的加速器也代理 quay 路径,需实测)。

- [x] **Step 2: 核验 15 个 quay.io 命名 base 全部在 rootless podman**

```bash
podman images --format '{{.Repository}}:{{.Tag}}' | grep '^quay.io/openeuler/openeuler' | sort
# 预期 15 行: {20.03,22.03,24.03}-lts + 全部 -lts-spN
```

### Task 4: 全 15 SP 烘焙镜像 + 构建 + 打包 + full 验证

**Files:** 无代码改动(用 build-all.sh 编排;submodule built/ 会更新)。

- [x] **Step 1: 串行跑 build-all --all --full**

```bash
cd /home/sdc/wangxu/opendcdiag-arm
./scripts/offline-build/build-all.sh --all --full 2>&1 | tee build-out/build-all-15.log
```

预期:15 行 `RESULT: PASS`(3 个 LTS 因 BUILD-HASH 变化——脚本 container-build.sh 改了,是哈希输入——会重建;其余 12 个全新构建)。每个 SP 内部:镜像烘焙(gcc 自检通过)→ 261/261 → list-tests 208 → pristine full `RESULT: PASS`。

预计耗时:15 × (烘焙 2-5min + 构建 ~4min + full 验证 ~6min) ≈ 3-4 小时。若中途个别 SP 失败,单独重跑该 SP 并记录真实失败原因,不得跳过或谎报。

- [x] **Step 2: 结果汇总核验**

```bash
grep -E '^RESULT:' build-out/build-all-15.log | sort | uniq -c
# 预期: 15 × "RESULT: PASS openEuler-XX.03LTS[_SPx]"
# 任何 FAIL → 回到该 SP 单独诊断修复后重跑
```

### Task 5: 提交产物 + 推送(主仓与 3 个 submodule)

- [x] **Step 1: submodule built/ 产物提交(每系列一个提交)**

```bash
for s in 20.03 22.03 24.03; do
  d=third-party/rpms/openEuler-$s
  git -C $d add -A
  git -C $d commit -m "built: refresh all 5 SP artifacts from 2026-09-02 full-matrix build

{LTS,SP1..SP4} each: 261/261 compile, list-tests 208, pristine full
verification PASS (eigen -n1 fail=0, full -n8 0 fail 0 crash)."
  git -C $d push origin main
done
```

- [x] **Step 2: 主仓 submodule 指针 + manifest 提交推送**

```bash
git add third-party/rpms/ scripts/offline-build/images/image-manifest.tsv
git commit -m "chore: 15-SP full build matrix green + RPM rpms/ migration

All 15 openEuler versions (3 series x LTS+SP1-4) built in containers and
passed full pristine verification. RPM trees restructured into rpms/
subdirectories; submodule pointers updated accordingly."
git push
```

- [x] **Step 3: 文档同步(大颗粒度修改)**

更新 `scripts/offline-build/README.md` 与 `docs/multi-version-build-deploy.md` 中涉及 RPM 目录结构的路径描述(README 里 `ls third-party/rpms/.../*.rpm` 示例等),确保 100% 反映 rpms/ 新结构。

## Self-Review 结论

- 覆盖检查:用户要求"每个 LTS 版本做一样的工作,一共 15 个镜像"→ Task 3/4;"RPM 迁移到 rpms/"→ Task 1/2;提交纪律→ 各 Task 的 commit 步骤;文档同步→ Task 5 Step 3。
- 无占位符:所有步骤含具体命令与预期输出。
- 类型一致性:rpms/ 路径在 Task 1 产出、Task 2 消费,变量命名统一。
- 风险点:①docker hub 个别 SP tag 可能缺失(缓解:实测确认 + quay 回退);②20.03 SP1-4 镜像体积大(1.2GB 级,拉取慢,后台并行);③build-all 对 12 个新 SP 是全新构建,哈希 skip 只对 3 个 LTS 生效。
