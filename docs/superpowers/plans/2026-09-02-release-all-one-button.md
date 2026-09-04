# 一键式 15 镜像全流程脚本(release-all.sh)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把已分散在人工操作/多脚本中的"15 个 openEuler 版本拉取 base 镜像 → 烘焙 → 构建 → 打包 → full 验证 → 提交推送"全流程收敛为**一个脚本** `scripts/offline-build/release-all.sh`,一条命令完成所有工作。

**背景(2026-09-02 已人工跑通的全流程,本脚本将其固化):**
1. base 镜像获取:docker hub(加速器)`docker pull openeuler/openeuler:<tag>` → `docker save` → `podman load` → `podman tag` 为 `quay.io/openeuler/openeuler:<tag>`(绕过 quay.io 直连挂起)。
2. 构建矩阵:`build-all.sh --all --full`(镜像烘焙幂等 skip + 源码哈希 skip;full 验证总是重跑)。
3. 结果校验:必须 15 × `RESULT: PASS`。
4. 提交推送:3 个 RPM submodule(built/ 产物,`push origin HEAD:main`,仅快进)+ 主仓(submodule 指针 + image-manifest.tsv,推当前非 main 分支)。

## Global Constraints

- 一键脚本必须**幂等**:base 已在 podman → skip;submodule/主仓已干净 → skip 提交;只有真正缺的东西才动。
- **安全闸**:主仓在 `main` 分支(或 detached)时**拒绝推送阶段**(CLAUDE.md:绝不推 main);submodule 推前 `merge-base --is-ancestor origin/main HEAD` 校验,防非快进覆盖。
- **失败即停**:`set -euo pipefail`,任何阶段失败退出非零并指明阶段;结果校验(15 PASS)不通过不得进入推送阶段。
- **不伪造**:脚本只报告真实命令输出;日志落 `build-out/release-all-<时间戳>.log`(gitignored)。
- sudo:base 拉取需要 docker(root)。脚本用 `sudo docker ...` 交互提示,不硬编码密码。

## 前置事实(已实测)

- 15 个 tag 全部存在于 docker hub(2026-09-02 实测逐个拉取成功)。
- `build-all.sh --all --full` 退出码即矩阵结论(FAIL_N=0 才 0);RESULT 行进 stdout。
- submodule 当前 detached HEAD,`push origin HEAD:main` + 快进校验是已验证的推送方式。
- `build-out/` 已 gitignore(本轮 build-all-15.log 从未出现在 git status)。

---

### Task 1: 实现 release-all.sh

**Files:**
- Create: `scripts/offline-build/release-all.sh`(可执行)

**Interfaces:**
- Usage: `release-all.sh [--skip-base] [--skip-build] [--skip-push] [-h]`
- 阶段:`preflight` → `stage_base`(15 tag 桥接)→ `stage_build`(build-all --all --full)→ `stage_check`(15 PASS)→ `stage_push`(3 submodule + 主仓)。
- 阶段跳过标志只影响对应阶段;默认全跑。

- [x] **Step 1: 写脚本**(结构:usage、公共函数 log/banner/die、preflight、stage_base、stage_build、stage_check、stage_push、main 按序执行,输出全程 tee 到 build-out/release-all-*.log)
- [x] **Step 2: `bash -n` 语法检查 + `--help` 实测**
- [x] **Step 3: preflight+base 阶段实测**(全部 15 个已在本地 → 预期 15 行 skip,秒级完成)

### Task 2: 全链路真实验证 + 提交推送

- [x] **Step 1: 完整跑一遍 `./scripts/offline-build/release-all.sh`**(后台;stage_build 哈希 skip 重建判定 + 15 × full 验证 ≈ 1.5-2h;stage_push 因全部已提交推送 → 预期报 clean skip,退出码 0)
- [x] **Step 2: 核对日志**(15 × RESULT: PASS;stage_push 输出各仓状态)
- [x] **Step 3: 主仓提交(脚本 + 本计划文件)并推送**(一个单元一个提交)

## Self-Review

- 覆盖:用户要求"一个脚本完成拉取/构建/验证/推送所有工作"→ Task 1 全阶段;验证真实性 → Task 2 全链路实测。
- 风险:①build-all 输出经 grep 管道缓冲,RESULT 行延迟——stage_check 在 build-all **退出后**再解析日志文件,不受缓冲影响;②docker sudo 交互——脚本在需要时提示,无密码缓存则由用户输入。
