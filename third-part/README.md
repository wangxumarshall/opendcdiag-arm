# third-part

OpenDCDiag (ARM64) 第三方依赖与离线构建 RPM 树。

本目录下的 `rpms/` 是一个 git submodule,指向 umbrella 仓 [opendcdiag-arm-rpm](https://github.com/wangxumarshall/opendcdiag-arm-rpm)。该 umbrella 仓本身不含 RPM,只通过三个嵌套 git submodule 聚合 openEuler 各大版本系列的 RPM 树,每系列一个独立子仓(拆分以避开 git 单 pack 2GB 上限):

| 子模块 | 远程仓 | 覆盖版本 |
|---|---|---|
| `rpms/openEuler-20.03/` | [opendcdiag-arm-rpm-20.03](https://github.com/wangxumarshall/opendcdiag-arm-rpm-20.03) | 20.03 LTS / SP1 / SP2 / SP3 / SP4 |
| `rpms/openEuler-22.03/` | [opendcdiag-arm-rpm-22.03](https://github.com/wangxumarshall/opendcdiag-arm-rpm-22.03) | 22.03 LTS / SP1 / SP2 / SP3 / SP4 |
| `rpms/openEuler-24.03/` | [opendcdiag-arm-rpm-24.03](https://github.com/wangxumarshall/opendcdiag-arm-rpm-24.03) | 24.03 LTS / SP1 / SP2 / SP3 / SP4 |

## 结构(两级嵌套 submodule)

```
opendcdiag-arm (主仓)
└── third-part/
    ├── eigen5/            # 仓内 Eigen 5(aarch64 构建用,详见主仓 CLAUDE.md)
    └── rpms/             # → opendcdiag-arm-rpm (umbrella)
        ├── openEuler-20.03/  (嵌套 submodule)
        ├── openEuler-22.03/  (嵌套 submodule)
        └── openEuler-24.03/  (嵌套 submodule, 含 SP3 基准版本)
```

克隆主仓时用 `git clone --recurse-submodules` 获取全部两级子模块。

## 用法

目标机离线安装对应 OS 版本的依赖:

```bash
# 进入对应版本子目录运行 install-deps.sh
cd third-part/rpms/openEuler-24.03/openEuler-24.03LTS_SP3
../../../../../scripts/offline-build/install-deps.sh .
```

## 各版本 RPM 数量

| 系列 | 版本 | RPM 数量 |
|---|---|---|
| 20.03 | LTS / SP1 / SP2 / SP3 / SP4 | 420 / 426 / 425 / 413 / 413 |
| 22.03 | LTS / SP1 / SP2 / SP3 / SP4 | 338 / 336 / 338 / 335 / 333 |
| 24.03 | LTS / SP1 / SP2 / SP3 / SP4 | 324 / 324 / 324 / 326 / 329 |

> `openEuler-24.03LTS_SP3` 是 OpenDCDiag ARM64 的基准构建版本,所有其他版本取与之同名的包集 + 完整依赖树。详见各子仓 README。
