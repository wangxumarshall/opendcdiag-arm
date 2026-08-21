# OFHC 研究报告（Open-Field-Health-Check）

> 整理日期：2026-08-20
> 研究对象：AMD Open Source Field Health Checks (OFHC)
> 部署环境：本机 ARM64 鲲鹏（CPU implementer 0x48 / part 0xd01，192 核，4 socket，无 SMT）
> 结论先行：OFHC 官方仅支持 AMD EPYC x86，本机 ARM64 不可直接运行；经改造后可运行，但其检测能力对 core 179 SDC 场景无实质增益。

---

## 一、OFHC 是什么

OFHC（Open Field Health Check）是 AMD 开源的**测试编排框架（test orchestration framework）**，不是测试本身。

官方文档（README.md）原话：

> OFHC run script is a test orchestration framework. You can give OFHC a configuration file with the tests to run and it'll run them in the desired sequence and log pass/fail information on each test. OFHC handles test execution on the desired cores of the CPU and checking for MCEs to make the process simpler for CPU test writers.

一句话定位：**它是"调度壳 + 记账员"，负责把测试撒到指定核上跑、查 MCE、记录 pass/fail。真正判断数据有没有损坏的是被调用的测试二进制本身。**

类比：
- OFHC = 考场（排座位、发卷、计时、收卷、登记成绩）
- 测试二进制（如 opendcdiag）= 考生做的题（实际运算、发现算错）

OFHC 单独运行不产生任何负载、不做运算、不判对错。必须由配置文件喂一个"测试二进制"给它。官方示例是 systester（圆周率计算器），本机无 systester，改用 opendcdiag。

---

## 二、官方架构限定（原文证据）

README 的 **Requirements** 明确锁死架构：

> - Python 3.8 or greater
> - **AMD EPYC CPU Family 25**
> - EDAC kernel MCA module
> - numactl
> - root permissions
> - Linux
> - bare metal non-virtualized environment
> - PyYaml

关键点：
1. **只认 AMD EPYC Family 25**（Zen3/Zen4 代服务器 CPU），无 ARM、无 Intel、无其他架构声明。
2. EDAC kernel MCA module、bare metal —— 全部指向 x86 EPYC 栈。

**官方从未声明支持 ARM。** 本机在 ARM64 上跑通是改造绕过官方架构假设的结果，不是 OFHC 官方支持。

---

## 三、OFHC 的两条检测通道

OFHC 把"检测"分成两类（README "Checking for MCE & Fails" 节）：

| 通道 | 谁负责 | 本机 ARM64 可用性 |
|------|--------|-------------------|
| **Fails**（测试失败/miscompare） | "User defined test is responsible for this" —— 测试二进制负责 | ✅ 可用（opendcdiag 判 miscompare → 非零退出码） |
| **MCE**（硬件机器检查异常） | OFHC 自己读 MSR 查 | ❌ 不可用（无 MSR，见下） |

### 3.1 通道 A：Fails（测试退出码）—— 本机可用

官方原话：

> It is expected that the test binaries will check for fails. When the executable detects a fail, it should return a non-zero exit code and provide any relevant details in STDOUT and STDERR.
> The executable should not control core affinity or any type of threading, the framework is expected to take care of this.

四步原理：
1. **生成核列表**：`CoreConfig` 调 `list_cores.sh`，列出要在哪些核上跑。
2. **逐核起进程**：`tests/Test.py` 的 `execTestOnCore` 用 `numactl --physcpubind=N <测试>`，每个核一个进程，`multiprocessing.Pool` 并行。
3. **测试自判 SDC**：被调用测试（opendcdiag）内部比对数据，发现不一致 → 报 fail → 退出码非 0。
4. **OFHC 读退出码**：`Test._posttest` 收每个核的 returncode，非 0 则记入 `failingACFCores`，写进 `cmd_results_list.log.csv`。

**OFHC 在这条通道上不产生任何检测能力，只是收集记录。** 真正判错的始终是测试二进制。

### 3.2 通道 B：MCE（硬件）—— 本机不可用

OFHC 的 MCE 检测**专给 AMD EPYC x86**：
- 打开 `/dev/cpu/{核号}/msr`（x86 MSR 字符设备）
- 读 AMD Zen 的 MCA bank 状态寄存器，地址写死：`MSR_MCG_CAP = 0x179`、bank 状态 `0xC0002000` 起
- 解析 `MCA_STATUS` 64 位位域（`val/uc/cecc/uecc/poison/deferred…`）

本机 ARM64 鲲鹏：
- 无 `/dev/cpu/N/msr` 设备（`ls /dev/cpu/` 不存在）
- 无 `/sys/devices/system/machinecheck/`（x86 machinecheck sysfs 缺失）
- ARM64 错误报告走 RAS/EDAC，不走 x86 MCA

**故通道 B 物理不可用。**

> 注：即便 MSR 可读，core 179 的 SDC 是**静默数据损坏**——数据错、无 MCE、dmesg 干净。所以通道 B 对此缺陷本就无效。参见记忆 `movbe-sdc-core179-findings`、`eigen-sdc-bitflip-position-stats`。

---

## 四、ARM64 改造记录

为让 OFHC 在本机跑通通道 A，做了最小侵入改造（保留 x86 原路径，只加 ARM64/sysfs 分支或桩）。

### 4.1 安装位置
- tar 包：`/home/sdc/root/arm64-sdc-fuzzing/Open-Field-Health-Check.tar.gz`（root-only 不可写）
- 解压到：`opendcdiag/Open-Field-Health-Check/`（用户可写）

### 4.2 改动文件清单

| 文件 | 问题 | 改动 |
|------|------|------|
| `system_config/cpuinfo.py` | 读 `/proc/cpuinfo` 的 `physical id`/`apicid`/`cpu cores`/`cpu family`（x86 字段），ARM64 无 → KeyError | 加 `_EnumerateSysfs()` 走 sysfs topology；加 `_is_x86_cpuinfo()` 探测；修 `GetSocketId`（原 `read(1)` 截断 2 位 package_id 如 19062） |
| `system_config/SystemConfig.py` | `_setupMCEDetection` 开 `/dev/cpu/N/msr` 崩；`_setCheckInterval` 写 machinecheck sysfs 崩；`_checkDependencies` 在 logger 初始化前用 `logger.warning` | `__init__` 用 `_hasMce = isdir(machinecheck) and isdir(/dev/cpu)` 门控 MSR；else `_mceChecker=None`、`isConstantMceChecking=False`；`checkMCEs/clearMCEs` 早返回；`_setCheckInterval` 无 sysfs 时 no-op；logger 默认初始化提前到 `_checkDependencies` 前；`_checkConfigIntegrity` 支持 `"all"` socket 字符串与 >2P |
| `list_cores.sh` | 无执行权限；假设 2P，4P 只列 socket0 的 48 核 | `chmod +x`；加 sysfs fallback：`SOCKET_TO_LIST=all` 且 `SOCKET_NUM!=2` 时读 `/sys/devices/system/cpu/online` 列全核 |
| `config_files/mrn_rmw-arm64.json` | 无 | 新建：opendcdiag mrn_rmw，args `--enable mrn_rmw --max-test-loop-count 0 -t {5s\|1m} -o <log>`，Core_Config `All:true` |

### 4.3 依赖安装
- `numactl` 原本缺失，`sudo dnf install -y numactl`（aarch64 包）安装。
- PyYAML 可选（用 JSON 配置）。

### 4.4 拓扑实测（CpuInfo 改造后）
```
arch         = arm64-sysfs
num_logical  = 192
num_physical = 192
num_sockets  = 4
ccd/socket  = 1
cores/ccd   = 48
smt          = False
socket(0)    = 36
socket(179)  = 19062
topology pkg = [36, 6378, 12720, 19062]
```
与 opendcdiag 自报的 core 179 topology（package 19062 / numa 7 / module 23340）一致。

### 4.5 运行方式
```bash
cd Open-Field-Health-Check
sudo -E python3 run.py config_files/mrn_rmw-arm64.json --log_dir logs/<name>
```
需 sudo（numactl 绑核 + root）。结果输出 `logs/<name>/cmd_results_list.log.csv`。

---

## 五、实测结果（OFHC 能否检测出 core 179 SDC）

### 5.1 配置
- 测试：opendcdiag `--enable mrn_rmw --max-test-loop-count 0 -t 1m`
- 核：全部 192 核（`All:true`）
- 模式：OFHC 对每个核起一个 `numactl --physcpubind=N` 单核 opendcdiag 实例，192 并行

### 5.2 结果（`cmd_results_list.log.csv`）
```
ACF = True                      ← 检测到应用层失败
ACF Failing Cores = ['179']     ← 精确定位 core 179
退出码 = 1
opendcdiag 在 core 179 连续 fail 4 次：
  time-to-fail: 8376.827 / 1935.841 / 16220.681 / 22258.309 (ms)
  seed: LCG:1796779244
  logical 179 / package 19062 / numa_node 7 / module 23340
  E> Failed at ../tests/cpu/misc/mrn_rmw.cpp:98: mrn_rmw data miscompare
MCE = False                     ← ARM64 无 MCE 通道，符合预期
```

**OFHC（改造后）能在此 ARM64 上通过通道 A 检测出 core 179 SDC，并自动定位 failing core。** 结果与 opendcdiag 原生报告完全一致。

### 5.3 检测耗时拆解
| 层级 | 耗时 | 含义 |
|------|------|------|
| OFHC 墙钟 | ~8 分钟 | 启动到 CSV 写出 |
| opendcdiag 核179实例运行 | ~470 秒 | 绑核 179 的实例实际运行（含 retry + 启动开销） |
| **首次失败** | **~8.4 秒** | 实例启动后 `time-to-fail: 8376ms` 首次 miscompare |

**关键：OFHC 不是在 miscompare 瞬间报错，而是等测试进程结束、读其退出码后才报告。** 检测延迟 ≈ 测试进程运行时长。

### 5.4 重要副产物：修正触发条件认知
OFHC 的 192×单核并发模型**同样触发了 core 179 SDC**。这修正了此前记忆 `core179-sdc-intermittent-dormant` 的结论——"单核 taskset 触发不了"指的是**单进程单核**；OFHC 是 **192 个单核进程同时满载**（系统级全核压力），照样触发。真正触发条件是**系统级全核满载压力**，与进程数/绑核方式无关。

---

## 六、OFHC 的能力边界（诚实评估）

### 6.1 OFHC 不能做的事
- **不能自己生成测试**：只会遍历配置文件里手写的参数列表（`ParamFactory`）。
- **不能"快速找到有效测试用例"**：README 的 Future Developments 连 "Test Randomization" 都还在规划中，当前无智能选测试能力。
- **不能在 ARM64 上查 MCE**：通道 B 物理不可用。
- **不能比测试二进制发现更多 SDC**：通道 A 的判错全靠被调测试，OFHC 不碰数据。

### 6.2 OFHC 能做、且 opendcdiag 原生也做的事
| 方面 | opendcdiag 原生 | OFHC |
|------|----------------|------|
| 跑测试 | 1 进程多线程全核 | 192 单核进程并行 |
| 报 failing core | cpu-mask 数 X 位置 | 直接 `['179']`，可读 |
| 全核压力 | 有 | 有（系统级满载） |

### 6.3 诚实结论
在本机 ARM64 + core 179 静默 SDC 的组合下，**OFHC 没有提供 opendcdiag 不具备的检测能力**。它检测出 SDC 靠的是 opendcdiag 的 miscompare 判定（通道 A），MCE 通道（B）架构性不可用且对此缺陷本就无效。OFHC 的价值在"组织/定位/记账"，不在检测能力本身。

若期待 OFHC 是"比 opendcdiag 更强的 SDC 探测器"或"能自动找有效测试"，此期待不成立——官方文档与源码均不支持。

---

## 七、可借鉴的工程做法

OFHC 官方虽仅支持 x86 EPYC，但其工程模式有若干条值得本 ARM64 SDC 研究借鉴：

### 7.1 "测试只管判错，框架只管绑核"分层（最值得借鉴）
官方原话："The executable should not control core affinity ... the framework is expected to take care of this." 测试二进制不碰亲和性，框架统一 `numactl --physcpubind`。opendcdiag 现在是自己一把抓全部核，绑核逻辑混在测试里。我们手写的 `taskset -c 179` 硬编码易错，可改为框架统一管绑核、测试二进制保持干净。

### 7.2 声明式笛卡尔积参数扫描（直接可抄）
OFHC 配置文件列 `Values`，框架自动做参数笛卡尔积遍历。我们之前做 H 探针 LINES 扫描、footprint sweep、mask/seed 扫描，全是手写 bash for 循环（`run_h_rate.sh`/`run_x_rate2.sh`/`run_xn_curve.sh` 等），每个脚本单独维护。改为声明式配置可省大量重复脚本。

### 7.3 cur_cmd 文件（崩了也能定位死在哪）
`logger.py` 在每个测试**开跑前**把命令行写进 `cur_cmd`，跑完再更新，另存 `cur_cmd.1`。机器 panic 后该文件残留，直接告诉你"死前正在跑哪个命令"。我们 round 脚本无此机制，事后只能 grep。零成本，出事时能救命。

### 7.4 failing-core 可读化输出
把 cpu-mask 的 X 位置解析成 `failing_cores=[179]` 一行输出。轻量改进，我们已有部分统计脚本在做，统一格式即可。

### 7.5 不建议借鉴的部分
- **MCE 通道**：ARM 无 MSR，且 core179 SDC 无 MCE，不抄。
- **192×单核模型**：对触发 SDC 不如 opendcdiag 单进程多线程（reschedule 产生跨核时序竞争）直接，不作为主力跑法。
- **list_cores.sh 的 CCD/Half/Quarter 划分**：AMD 拓扑概念，ARM 无 CCD，靠 sysfs fallback 才跑通，不抄其划分逻辑。

### 7.6 最该抄的一条
把散落的 `run_*.sh`、probe 脚本重构成"配置文件声明参数 + 框架统一绑核 + cur_cmd 防丢失 + failing-core 解析"的轻量流水线：换测试改配置不改脚本，换核组不硬编码 taskset，跑挂能定位，出结果自动知 failing core。即保留 OFHC 适合本研究的部分，扔掉 x86/MCE 部分，做一个 ARM64 轻量编排器。

---

## 八、相关记忆与文档
- `movbe-sdc-core179-findings`：core 179 SDC 确认（静默损坏，无 MCE）
- `eigen-sdc-bitflip-position-stats`：位翻转统计（mantissa 85-93%，符号位免疫）
- `core179-sdc-intermittent-dormant`：触发条件（全核 + auto seed；OFHC 192×单核模型进一步修正"单核触发不了"的表述）
- `ofhc-arm64-port`：本次改造的精简记忆
- `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT_V3.md`：core 179 SDC 定位报告 v3
- `docs/CORE179_SDC_REPORT_CN.md`：core 179 SDC 中文报告
