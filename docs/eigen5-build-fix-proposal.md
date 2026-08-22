# 方案：让 ARM64 构建默认使用仓内 Eigen 5（替代系统 Eigen 3）

> 状态：已部分实施（补丁 1 已提交 `f281967`）。本文档是完整方案的总览与决策记录，
> 指出当前补丁 1 的遗留隐患，并给出补丁 2 作为收尾。
> 目标机器：Kunpeng 920 / openEuler 24.03 SP3，aarch64，GCC 12.3.1。

## 一、问题

在 aarch64 上执行 `meson setup builddir --buildtype=release`，当未手动设置
`PKG_CONFIG_PATH=./third-part/eigen5` 时，构建在 `tests/cpu/meson.build` 处硬失败：

```
Run-time dependency eigen3 found: NO  (tried pkg-config)
tests/cpu/meson.build: ERROR: Dependency "eigen3" not found (tried pkg-config)
```

## 二、根因（三个层次，由浅入深）

### 层次 1 —— 表面：构建脚本无条件走系统 pkg-config

`tests/cpu/meson.build` 原始写法：

```meson
eigen3_dep = dependency('eigen3', include_type : 'system', static : dep_static)
```

它直接向系统 pkg-config 查找 `eigen3`，**没有任何回退**。在未安装系统 Eigen 的
ARM64 环境（或忘了导出 `PKG_CONFIG_PATH`）下，必然报"Dependency not found"。

### 层次 2 —— 仓内 pkg-config 文件本身是坏的（硬编码绝对路径）

仓内 `third-part/eigen5/eigen3.pc` 内容（被 git 跟踪，`9f55155` 引入）：

```
prefix=/home/sdc/opendcdiag/third-part/eigen5   ← 注意：指向"参考仓 opendcdiag"，不是本仓 opendcdiag-arm
exec_prefix=${prefix}
...
Cflags: -I${prefix}
```

即便按 `CLAUDE.md` 加了 `PKG_CONFIG_PATH=./third-part/eigen5`，pkg-config 解析出的
`-I` 实际指向隔壁参考仓 `/home/sdc/opendcdiag/third-part/eigen5`。该路径今天碰巧存在，
但参考仓一旦被删/移动/更名，文档里的工作流就**静默失效**。这个 pc 文件不可重定位，
绑定到一台特定机器的特定目录。

### 层次 3 —— 最关键：系统 Eigen 3.3.x 在 ARM64 + GCC 12 上是坏的，却会被静默选中

这台机器**安装了系统 Eigen 3.3.8**（`/usr/share/pkgconfig/eigen3.pc`）。实测：

```
$ g++ -std=gnu++23 $(pkg-config --cflags eigen3) -c eigen_inc_test.cpp
/usr/include/eigen3/Eigen/src/Core/PlainObjectBase.h:905: warning:
  bitwise operation between different enumeration types ... is deprecated
  [-Wdeprecated-enum-enum-conversion]
```

这正是 `CLAUDE.md` 所述 "system Eigen 3.3.x breaks on GCC 12+" 的实证。在框架的
`-Wextra` 语义下这些 deprecation 会升级为硬错误。

**结论**：如果只做"找不到才回退"（层次 1 的朴素修法），在系统装有 3.3.8 时，
回退**根本不会触发**，照样用坏的 3.3.8。因此正确策略必须是 **aarch64 上无条件优先
仓内 Eigen 5.0.1**，而不是 fallback。

## 三、已实施方案：补丁 1（已提交 `f281967`）

在 `tests/cpu/meson.build` 按架构分流：

```meson
# On aarch64, system Eigen 3.3.x breaks on GCC 12+ (...). The repo
# ships Eigen 5.0.0+ under third-part/eigen5 for this exact reason, so prefer
# it unconditionally on ARM64 and make the build self-contained (...). On x86-64
# keep the historical behaviour of looking up the system eigen3 via pkg-config.
if host_machine.cpu_family() == 'aarch64'
    eigen3_dep = declare_dependency(
        include_directories: include_directories(
            '../../third-part/eigen5',
            is_system : true,
        ),
    )
else
    eigen3_dep = dependency('eigen3', include_type : 'system', static : dep_static)
endif
```

aarch64 走 `declare_dependency` 直接指本仓 `third-part/eigen5`，**完全不经过
pkg-config**，因此：
- 不再需要 `PKG_CONFIG_PATH=./third-part/eigen5`；
- 坏的系统 3.3.8 无法再被静默选中；
- x86-64 维持原 pkg-config 查找，路径不变（additive port 原则）。

### 补丁 1 的真实验证（均在不设 `PKG_CONFIG_PATH` 下执行）

| 检查 | 命令 | 结果 |
|---|---|---|
| 配置 | `meson setup builddir_fb --buildtype=release`（干净 builddir） | 通过；日志中**无** `eigen3` pkg-config 行（走 declare_dependency） |
| 构建 | `ninja -C builddir_fb` | exit 0；eigen_gemm/svd/jacobi/svd_cdouble_sve 全部编出 |
| 功能 | `./opendcdiag -e eigen_svd_double -t 2000 -n 1` | `exit: pass` |
| 回归 | `./opendcdiag -e zstd19 -t 2000 -n 1` | `exit: pass`，无连带破坏 |
| x86 不变 | diff | 改动仅在 `#if aarch64`/`else` 分支，x86 `dependency('eigen3')` 原样保留 |

## 四、补丁 1 的遗留隐患与收尾方案：补丁 2

补丁 1 让 aarch64 的 meson 构建**绕开**了那个坏 pc 文件，但**没有修复 pc 文件本身**。
该隐患仍在：
- `third-part/eigen5/eigen3.pc` 仍是硬编码 `/home/sdc/opendcdiag/...` 的坏文件；
- 任何**不经过 meson** 的下游消费者（外部工程、`pkg-config --cflags eigen3`、
  IDE、文档示例）若设了 `PKG_CONFIG_PATH=./third-part/eigen5`，仍会拿到指向参考仓的
  错误 `-I` 路径，且参考仓消失时静默失效。

### 补丁 2：把 `eigen3.pc` 改为可重定位（独立提交）

将 `third-part/eigen5/eigen3.pc` 的 `prefix` 由硬编码绝对路径改为 pkg-config 内置
变量 `${pcfiledir}`：

```
- prefix=/home/sdc/opendcdiag/third-part/eigen5
+ prefix=${pcfiledir}
  exec_prefix=${prefix}
  ...
  Cflags: -I${prefix}
```

`${pcfiledir}` 在 pkg-config 解析时展开为 **pc 文件自身所在目录**，即
`<repo>/third-part/eigen5`，与仓库实际位置无关 → 仓库可被 clone 到任意路径都正确。

### 补丁 2 的真实验证（已用临时替换实测）

```
# 临时把 prefix 改成 ${pcfiledir} 后：
$ PKG_CONFIG_PATH=<repo>/third-part/eigen5 pkg-config --cflags eigen3
-I/home/sdc/opendcdiag-arm/third-part/eigen5        ← 本仓正确路径（不再是隔壁参考仓）

$ g++ -std=gnu++23 $(... pkg-config --cflags eigen3) -c eigen_inc_test.cpp
（编译 exit 0）
```

替换测试后已还原原文件，工作树干净。

### 补丁 2 的合规性核对（补丁纪律 / x86-64 不变规则）

- **一补丁一单元**：补丁 2 只动 `third-part/eigen5/eigen3.pc` 一个文件、一行，与补丁 1
  （meson 构建逻辑）是不同单元，分开提交。
- **x86-64 不变**：pc 文件改动与架构无关，对 x86 与 aarch64 对称生效；且 aarch64 构建已
  在补丁 1 里绕开 pc，故补丁 2 对 aarch64 meson 构建无行为变化，只修复外部消费者路径。
- **自验证**：实施补丁 2 时需 `PKG_CONFIG_PATH=./third-part/eigen5 pkg-config --cflags eigen3`
  确认解析为本仓路径，并跑一次 `meson setup --reconfigure + ninja` 确认 aarch64 构建仍
  exit 0（回归）。

## 五、不采用的备选方案及理由

| 备选 | 为什么不采用 |
|---|---|
| A. 只在 meson 里加 `required:false` + fallback 到仓内 eigen5 | 系统装了 3.3.8 时 fallback 不触发，照样用坏的 3.3.8（层次 3）。实测否决。 |
| B. `PKG_CONFIG_PATH=./third-part/eigen5` 写进文档/CI | 治标不治本：坏 pc 文件仍指向隔壁参考仓（层次 2），且依赖人工记忆，不可重定位。 |
| C. 用 meson wrap / subproject 包装 eigen5 | 改动面大，需引入 wrap 文件与目录结构重排，超出"修构建"范畴；当前 vendored 形式（1914 文件直入 git）已可用，无必要。 |
| D. 删掉 `third-part/eigen5/eigen3.pc`，只留 `.pc.in` | 会破坏所有靠 `PKG_CONFIG_PATH` 消费的外部用户；且补丁 2 一行 `${pcfiledir}` 即可同时修好，更小侵入。 |

## 六、实施清单

- [x] **补丁 1**（`f281967`，已推送）：`tests/cpu/meson.build` aarch64 分流到仓内 eigen5。
- [ ] **补丁 2**（待实施）：`third-part/eigen5/eigen3.pc` 改 `prefix=${pcfiledir}`，单独提交。
  实施后验证：`pkg-config --cflags eigen3` 解析为本仓路径 + `ninja` 回归 exit 0。
