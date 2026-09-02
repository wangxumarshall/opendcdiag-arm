# third-party/rpms

SDCShield (ARM64, derived from OpenDCDiag) 离线构建依赖 RPM 树。

本目录通过三个 git submodule 聚合 openEuler 各大版本系列的 RPM 树,每系列一个独立子仓(拆分以避开 git 单 pack 2GB 上限):

| 子模块 | 远程仓 | 覆盖版本 |
|---|---|---|
| `openEuler-20.03/` | [sdcshield-rpm-20.03](https://github.com/wangxumarshall/sdcshield-rpm-20.03) | 20.03 LTS / SP1 / SP2 / SP3 / SP4 |
| `openEuler-22.03/` | [sdcshield-rpm-22.03](https://github.com/wangxumarshall/sdcshield-rpm-22.03) | 22.03 LTS / SP1 / SP2 / SP3 / SP4 |
| `openEuler-24.03/` | [sdcshield-rpm-24.03](https://github.com/wangxumarshall/sdcshield-rpm-24.03) | 24.03 LTS / SP1 / SP2 / SP3 / SP4 |

## 结构(submodule)

```
sdcshield (主仓)
└── third-party/
    ├── eigen5/               # 仓内 Eigen 5(aarch64 构建用,详见主仓 CLAUDE.md)
    └── rpm/                  # 本目录
        ├── openEuler-20.03/  (submodule → sdcshield-rpm-20.03)
        ├── openEuler-22.03/  (submodule → sdcshield-rpm-22.03)
        └── openEuler-24.03/  (submodule → sdcshield-rpm-24.03, 含 SP3 基准版本)
```

克隆主仓时用 `git clone --recurse-submodules` 获取全部子模块。

## 用法

目标机离线安装对应 OS 版本的依赖:

```bash
# 进入对应版本子目录运行 install-deps.sh
cd third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3
../../../../scripts/offline-build/install-deps.sh .
```

## 各版本 RPM 数量

| 系列 | 版本 | RPM 数量 |
|---|---|---|
| 20.03 | LTS / SP1 / SP2 / SP3 / SP4 | 420 / 426 / 425 / 413 / 413 |
| 22.03 | LTS / SP1 / SP2 / SP3 / SP4 | 338 / 336 / 338 / 335 / 333 |
| 24.03 | LTS / SP1 / SP2 / SP3 / SP4 | 324 / 324 / 324 / 326 / 329 |

> `openEuler-24.03LTS_SP3` 是 SDCShield ARM64 的基准构建版本,所有其他版本取与之同名的包集 + 完整依赖树。详见各子仓 README。
