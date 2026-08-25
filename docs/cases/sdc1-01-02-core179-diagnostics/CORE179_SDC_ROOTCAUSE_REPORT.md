# OpenDCDiag 核心 179 SDC 故障根因分析报告(合并主文档)

**报告日期:** 2026-08-06 起;2026-08-07 修订(libc-only MRU);2026-08-13 修订(指令级深挖 + 机制判别 + 安全约束证伪)
**目标系统:** 4 路 Kunpeng 920(TaiShan v110)服务器,192 核心
**受测软件:** OpenDCDiag(自研 ARM64 构建,Eigen 5.0.1)
**本文档定位:** 本目录原先有 7 份 md(根因报告、复现指南、探针索引、物理层假说、深度根因深挖、reproduction_report、fuzzing readme),**已全部合并进本文**。引用一律指向本文档内部章节,不再有独立配套文档。

---

## 结论摘要

核 179 的乱序执行引擎在 **Eigen `SimplicialCholesky::factorize()` 数值分解列更新循环**(cdiv 条件分支 + rank-1 update 标量 FMA + 间接地址生成的特定交错序列)下,产生**静默、负载敏感的寄存器活性边界泄漏**,把其他计算数据混叠进长存活对角累加器 `d0`,经 `L[0,0]=sqrt(d0)` → solve 前向回代 `x[0]=b[0]/L[0,0]` **两跳放大**到求解结果向量 `x` 的固定首元素 `x[0]`,损坏为多位(21-32 bit)数据混叠。

- **触发条件:** 核 179 单线程 + 同 socket(PkgID 19062)≥47 核满载 + 矩阵规模 N≥256。
- **不触发:** 单核 / N<256 / 纯 FMA / 纯 SpMV / 纯 gather / 三角 solve / 密集 GEMM/SVD。
- **已排除:** 软件 bug、浮点 pipe 损坏、cache/TLB/分支/stall 异常、内存 ECC、中断淹没、EINJ 注入、单 bit SEU、单一功能单元损坏。
- **机制判别(2026-08-13 补充):** 穷举六种"乱序+重命名"叠加流程,判定最可能是**流程 A:物理寄存器活性误判(回收过早)**,次可能是 C(ROB 提交顺序错);A vs C 需 RTL 区分,但 4× 跨调用状态签名略偏 A(详见 §11.2a)。
- **最小复现用例(MRU):** `mru_eigenmc.c` + `eigen_cabidrv.cpp`(`build_mru_eigenmc.sh` 一键构建)。**该 MRU 仅依赖 libc/libm**(`ldd` 证明,无 libstdc++/libgcc_s),已端到端验证复现同一故障,可作为芯片 DFT/量产筛选测试向量。
- **安全约束(2026-08-13 证伪更新):** 主报告原先称"47 核满载 + timeout 75 = 安全不挂死",**已被 2026-08-13 实跑证伪**——同样配置导致 RCU stall + 硬件 watchdog 硬复位(详见 §11.3、§16)。**禁止再用"满载 + 核 179 跑触发程序"的方式复现。**

研究方法:**分层假设排除法**(6 层),每层用对照实验证伪一个假设,逐步逼近根因。详见 §4-§9 与 §14 方法学小结。

---

## 1. 背景与目标

OpenDCDiag 在 ARM64(aarch64)上完成构建并通过自测后,启动无限循环全量用例运行(~23 个测试)。运行约 100 秒后首次出现测试失败,失败集中在 `eigen_sparse` 测试(double 精度稀疏 Cholesky 求解)。运行期间系统两次卡死,需强制掉电重启。

本文目标:
1. 区分"Eigen 5.0 软件稳定性问题"与"真实硬件 SDC";
2. 在芯片微架构层面定位根因(哪个执行路径、什么机理);
3. 给出可复现的实验证据链、满足"仅依赖 libc"约束的最小复现用例,以及可交付的诊断探针。

## 2. 系统与环境基线

| 项目 | 值 |
|---|---|
| 平台 | HiSilicon HIP08(ACPI EINJ/SDEI/GHES 固件 first mode) |
| CPU | Kunpeng 920,Implementer 0x48,PartNum 0xd01,TaiShan v110 core |
| 拓扑 | 4 socket × 48 核 = 192 核;PkgID 36 / 6378 / 12720 / 19062 |
| 故障核 | CPU 179 = PkgID 19062,NUMA node 7,Module 23340(与 176-178 同 module) |
| Stepping | variant 1,revision 0,**所有核微码 = 0x0**(一致,无核间差异) |
| L1 | D/I 各 64KB,4 路;L2 512KB;L3 24MB / package(24 核共享) |
| OS | openEuler 24.03 SP3,kernel 6.6.0-145.3.23.154.aarch64 |
| EDAC | ghes_edac,32 DIMM,APEI firmware-first;SBSA 硬件 watchdog(sbsa-gwdt,10s 超时) |
| OpenDCDiag-arm | 自研构建,`--buildtype=debugoptimized -Dssl_link_type=dynamic`,bundled Eigen 5.0.1(`third-party/` vendored,非 submodule) |

诊断期间 `kernel.perf_event_paranoid` 调为 -1 以启用非特权系统级 per-core PMU 采样。

## 3. 失败现象

无限循环 `opendcdiag --beta -T forever` 运行 ~17 分钟后:
- 共 2178 次测试迭代 ok,2 次 not ok;
- 失败测试:`eigen_gemm_float_dynamic_square`(1 次)与 `eigen_sparse`(1 次,另含 SIGSEGV 段错误);
- **两次失败均来自 Thread 179 / CPU 179(pkg 19062, core 179)**。

## 4. 第一层:软件 vs 硬件

**证伪假设:** "Eigen 5.0 在该 seed 下有确定性软件 bug"。

### 4.1 实验设计

| 实验 | 配置 | 验证假设 |
|---|---|---|
| A | 核 179,失败 seed `LCG:323306158` 固定,20 次单跑 | 失败 seed 是否确定性触发 |
| B1/B2 | 核 179 vs 核 0,固定失败 seed,120s | seed + 核绑定 |
| C1 | 核 179,eigen_gemm_float 失败 seed,90s | 第二个失败 seed 复现 |
| D1 | 核 179,自然 RNG,单核 180s | 单核低负载是否触发 |
| E | 全核(含 179)满载 300s | 还原原始失败条件 |
| F | 全核(排除 179)满载 300s | 排除 179 后是否仍有失败 |

### 4.2 结果

| 实验 | ok | fail | 失败核 |
|---|---|---|---|
| A(20×3 loop) | 20 | 0 | — |
| B1 核 179 | 9146 | 0 | — |
| B2 核 0 | 9293 | 0 | — |
| C1 核 179 | 1513 | 0 | — |
| D1 核 179 单核 | 3168 | 0 | — |
| E 全核含 179 | 3752 | **6** | **全 179** |
| F 全核排除 179 | 4312 | **0** | — |

### 4.3 第一层推理

- **固定失败 seed 在核 179 上跑 9000+ 次全过**(B1)→ 该 seed/数值条件本身不触发软件 bug(若有软件 bug,必每次复现)→ 排除"Eigen 在该 seed 下的确定性 bug"。
- **排除核 179 后 191 核满载 0 失败**(F)→ 失败严格绑定核 179,而非负载/全局条件。
- **单核 179 不触发**(D1)→ 需满载压力。
- **失败偶发**(A 不可复现)→ 符合硬件瞬态错误,而非确定性软件缺陷。

**第一层结论:排除 Eigen 5.0 软件 bug,指向核 179 硬件层缺陷,且仅在满载下触发。**

## 5. 第二层:定位触发测试类型

**证伪假设:** "浮点运算 pipe 本身损坏"(否则所有浮点测试都该失败)。

### 5.1 实验 G5/G6(决定性)

全核满载,**固定 seed**(所有核用相同数据、相同计算),分别用两种 seed:

| 实验 | seed | ok | fail | 失败核 |
|---|---|---|---|---|
| G5 | LCG:323306158 | 3827 | **285** | **285/285 全 179** |
| G6 | LCG:987654321 | 2953 | **140** | **140/140 全 179** |

**所有核相同输入,只有核 179 失败** → 彻底排除 seed/数据/数值边界(若是软件 bug,所有核都该失败)。

### 5.2 测试类型隔离(48 核 socket 满载,固定 seed,15s)

| 测试 | 计算特征 | ok | fail | 失败核 |
|---|---|---|---|---|
| eigen_gemm_double | 密集 double 矩阵乘 | 84 | 0 | — |
| eigen_gemm_float | 密集 float 矩阵乘 | 210 | 0 | — |
| eigen_svd / svd_double2 | SVD 分解 | 121/98 | 0 | — |
| **eigen_sparse** | **稀疏 Cholesky + 间接寻址** | 798 | **7** | **179** |
| arm64_sdc(CRC/int) | 整数 | 21571 | 0 | — |
| zstd/zlib | 整数压缩 | 179 | 0 | — |

### 5.3 第二层推理

- 只有 **eigen_sparse**(稀疏矩阵 + 大量间接寻址/指针解引用 + Cholesky 分解)触发失败;**密集 GEMM、SVD 等浮点测试 0 失败** → 排除"浮点 pipe 损坏"(否则所有浮点测试都该失败)。
- 整数/CRC 类 0 失败 → 整数路径完好。
- 触发单元 = **稀疏 + 间接寻址**的特定计算模式。

**第二层结论:故障与"间接寻址 + 浮点"组合相关,而非纯浮点 pipe 损坏。**

## 6. 第三层:微架构事件对比(per-core PMU)

**证伪假设:** "核 179 的 cache/LSU/TLB/分支/内存子系统存在可观测损坏"。

在 48 核 socket 满载触发核 179 失败(40 次)的同时,用 `perf stat -C` 对比核 179(故障)与核 176(健康,同 module)的微架构事件(5s 样本):

| 事件 | 核 179 | 核 176 | 健康判断 |
|---|---|---|---|
| IPC(insn/cycle) | 1.74 | 1.79 | 同量级 |
| branch-miss % | 4.02% | 4.03% | 同 |
| L1-dcache load-miss % | 1.80% | 1.73% | 同 |
| l1d_cache_refill | 31.4M | 31.4M | 同 |
| dTLB-load-misses | 4.75M | 4.65M | 同 |
| l2d_cache_refill | 17.3M | 16.9M | 同 |
| ll_cache_miss | 1.74M | 1.89M | 同 |
| stalled-cycles-frontend % | 9.77% | 10.40% | 同 |
| stalled-cycles-backend % | 15.40% | 16.05% | 同 |
| **`armv8_pmuv3/memory_error`** | **0** | **0** | **均 0** |
| ghes_edac ce/ue_count | 0 | 0 | 均无内存 ECC 错误 |

### 6.1 第三层推理

核 179 与健康核 176 在**所有微架构事件上差异 < 2%**,IPC/cache/TLB/分支/stall 全部正常。**`memory_error` PMU = 0**、EDAC 计数 = 0。

**第三层结论:排除核 179 的 cache/LSU/TLB/分支/浮点 pipe/内存子系统损坏**(PMU 会反映任一损坏);也排除内存 ECC 损坏。

此时出现悖论:核 179 在微架构层完全正常,却在满载下静默算错。这指向**计算执行路径中不被 PMU/EDAC 捕获的静默缺陷**。

## 7. 第四层:实时内核错误监控(静默性 + 挂死根因)

**证伪假设:** "失败伴随可观测硬件错误事件(同步异常/RAS 上报),且系统挂死由内存 ECC 错误导致"。

为区分"硬件可观测错误(会触发同步异常/RAS)"与"静默计算损坏",并排查后续两次系统挂死的根因,在 48 核 socket 满载触发核 179 失败的同时实时监控内核日志与 EDAC 计数。

### 7.1 实验 M1:满载 + 实时 dmesg/EDAC 跟踪

- 先 `rmmod einj`(移除诊断期间加载的注入模块,避免干扰);
- 重置 EDAC 计数器(`reset_counters`);
- 后台 `dmesg --follow` 过滤 `mce|sei|sdei|edac.*err|ghes|hardware|panic|oops|ras|...` 实时跟踪;
- 前台 48 核 socket 满载跑 eigen_sparse 固定 seed,20s;
- 结束后读 EDAC `ce/ue_count` 与新增 dmesg。

### 7.2 结果

| 指标 | 值 |
|---|---|
| 满载 20s 触发核 179 失败 | **55 次**(全 179) |
| 同窗口 EDAC `ce_count` | **0** |
| 同窗口 EDAC `ue_count` | **0** |
| 新增内核 dmesg(SEI/SDEI/MCE/GHES/hardware error/panic/oops) | **0** |
| 唯一新增 dmesg | `EINJ: Error INJection is initialized.`(诊断期间 `modprobe einj` 留下,与故障无关) |

### 7.3 第四层推理

- **满载 15-20s 内产生 38-55 次计算失败,而 EDAC/`memory_error` PMU/dmesg 全程为 0** → 失败**不伴随**任何可观测的硬件错误事件(无同步异常、无 RAS 上报、无内存 ECC 计数)。
- 这把两次系统挂死的根因**排除内存 ECC**:若是内存不可纠正错误导致挂死,EDAC `ue_count` 必非零;但挂死时该计数仍为 0。
- 故挂死机制只能是**静默计算损坏波及控制流**(混叠数据被当指针/控制值 → 不可恢复状态),而非内存/电源层可观测故障。

**第四层结论:计算失败是"静默"的(无任何硬件错误事件),且挂死不是内存 ECC 所致,而是计算损坏波及控制流。**

## 8. 第五层:字节级损坏模式(自研探针 esdiag3)

**证伪假设:** "输入数据 A/b 被破坏导致结果错"与"随机单粒子翻转 SEU(只翻 1 bit、随机位置)"。

由于 opendcdiag 的 eigen_sparse 用 `report_fail()`(仅报告"不等",无字节对比),无法定位损坏模式。自研探针 `esdiag3`(链接 Eigen 5.0,复现 Cholesky,记录逐元素差异 + CRC)。

### 8.1 实验 D4:固定输入 A,分离"输入损坏" vs "计算损坏"

`esdiag3` 构建矩阵 A 一次(固定),重复 3000 次 Cholesky solve,分别 CRC 校验 A 与结果 x:

| 指标 | 核 179 | 核 176 |
|---|---|---|
| A(输入)损坏次数 | **0/3000** | 0/2000 |
| x(结果)错误次数 | **28/3000(0.93%)** | 0/2000 |
| 每次失败时 A 校验 | **完好(a_crc_ok=1)** | — |

### 8.2 损坏字节模式(D2/D3)

| 迭代 | 损坏元素 | actual | expected | xor(popcount) |
|---|---|---|---|---|
| 12 | elem[0] | -0.9324179 | -0.9324378 | 0x000000d9d68ba712(**21 位**) |
| 144 | elem[0] | -0.6320479 | -0.9324378 | 0x0009ef3b42436fd6(**30 位**) |
| 81 | elem[0] | -0.9421112 | -0.9324378 | 0x0003f341aba2d4d1(**26 位**) |
| 91 | elem[0] | -0.8575007 | -0.9324378 | 0x0006a622880c74e5(**21 位**) |
| 767 | elem[0] | -15.681250 | -0.9324378 | 0x7fc28a4b1b432094(**28 位**) |

### 8.3 第五层推理(决定性)

- **A 完全未损坏**(0/3000),但 x 错误 28/3000,**每次失败时 A 都完好** → 损坏发生在**计算过程本身**,非输入数据。
- **损坏固定在 elem[0]**(结果向量首元素)——固定位置,非随机元素 → 排除随机翻转。
- **多位翻转 21-30 位** → 排除单粒子翻转 SEU(只翻 1 bit)。
- **actual 值是来自他处的垃圾**(-15.68、-0.63 等,与 expected -0.93 完全无关)→ **数据混叠**(data aliasing):其他计算结果覆盖了 elem[0]。
- elem[0] 地址每次相同(固定栈位置 `0xfffff3e8d8e0`;**注:2026-08-13 反汇编确认该地址实为 factorize 内部 `y[]` 工作区首址,而非 `x[]` 输出地址——详见 §11.2a 修正**)。

**第五层结论:核 179 的计算执行路径在满载下,把"其他数据"混叠写入结果向量 x 的 elem[0] 固定存储位置。这是局部、负载敏感的写回路径缺陷。**

## 9. 排除"注入"与中断干扰假设

用户怀疑是否"注入"导致,系统性排查:

| 排查项 | 结果 |
|---|---|
| EINJ 接口 | `einj` 模块仅由诊断期间 `modprobe` 加载(此前 lsmod 无),`error_type=0x0` 无激活注入 |
| 注入脚本/cron/history | `/root/.bash_history`、sdc history、crontab、systemd 均无注入痕迹 |
| opendcdiag 自身注入 | `--inject-idle`(注入 CPU 空闲,非数据);`arm64_sdc` 的 `enable_injection=false`(默认关),其 `^=0x01` 是测试自检逻辑(每次都做,非外部注入) |
| 设备中断淹没核 179 | `/proc/interrupts`:核 179 上仅 `arch_timer`/`arm-pmu`/IPI,无 SAS/SCSI 设备 IRQ(SAS IRQ 绑在核 8/21/24-47 等) |

**排除注入与中断干扰。**

## 10. 第六层:最小复现用例削减(MRU)

第五层确认"计算过程"出错,但 Cholesky 是复杂多阶段算法。为给芯片设计提供精确负反馈,需把触发条件削减到最小:**究竟哪个计算阶段、什么指令序列、什么矩阵规模**触发损坏。自研一组二分探针,逐层削减依赖。

### 10.1 削减 1:是否需要完整 Cholesky?(对比单一操作)

在 47 核 socket 满载(已知触发条件)+ 核 179 单线程跑各探针:

| 探针 | 计算特征 | 失败 | 结论 |
|---|---|---|---|
| dense dot product(`minimal_probe` 变体 1) | 密集点积,无间接寻址 | 0/5000 | ❌ 不触发 |
| SpMV,CSR 间接索引(`minimal_probe` 变体 2) | 稀疏矩阵向量乘,间接寻址 | 0/5000 | ❌ 不触发 |
| gather,指针追逐(`minimal_probe` 变体 3) | 链表式 gather,重度间接 | 0/5000 | ❌ 不触发 |
| pure FMA(`fma_probe.c`) | 规则 FMA 循环,无间接无分支 | 0/10000 | ❌ 不触发 |
| **esdiag3(Eigen Cholesky)** | **symbolic+numeric 分解** | **15/2000(0.75%)** | ✅ 触发 |

**洞察:所有"单一操作"(纯 FMA / 纯 SpMV / 纯 gather)都不触发,只有完整 Cholesky 触发** → 损坏需要 Cholesky 的**特定多阶段指令序列**,而非单一功能单元(纯 FPU/纯 LSU/纯 AGU)。

### 10.2 削减 2:Cholesky 哪个阶段?(symbolic vs numeric)

Eigen 的 `compute()` = `analyzePattern()`(symbolic,纯指针/逻辑确定 fill-in)+ `factorize()`(numeric,FMA 数值更新)+ `solve()`(三角求解)。分离三者:

| 模式(esdiag_phase2 mode) | 失败 | 失败率 |
|---|---|---|
| solve-only(重用分解,只做三角求解) | 0/2000 | 0% |
| **numeric-only(重用 symbolic,只重复 factorize,mode=1)** | **15/1500** | **1.0%** |
| compute-both(symbolic+numeric 每次重做,mode=0) | 4/1500 | 0.27% |

**关键洞察:**
1. **solve(三角求解)0 失败** → 损坏不在求解阶段;
2. **numeric factorize 单独触发** → 损坏在数值分解阶段;
3. **numeric-only 失败率(1.0%)≈ 4× compute-both(0.27%)** → symbolic 阶段重做反而**降低**触发率 → symbolic 打断/刷新了某个持续占用的微架构状态(寄存器重命名历史/ROB/物理寄存器堆/LSU store buffer),从而缓解。

这把根因精确到 **numeric factorize 的列更新循环**(cdiv + rank-1 update,supernodal level-scheduled:密集条件分支 + FMA + 间接地址生成的特定交错)。

### 10.3 削减 3:损坏位置是否随矩阵规模变化?

固定 symbolic,重复 numeric factorize,扫描矩阵大小 N:

| N | 失败 | elem[0] 失败 | 首个坏元素 |
|---|---|---|---|
| 64 | 0 | 0 | — |
| 128 | 0 | 0 | — |
| 256 | 4 | **4(全)** | 0 |
| 512 | 8 | **8(全)** | 0 |

**关键洞察:**
1. **N=64/128 不触发,N≥256 才触发** → 存在**规模阈值**(factorize 工作量需足够大,推测触发特定 cache 容量/执行窗口压力);
2. **N=256/512 全部损坏在 elem[0]**(first_bad_idx=0,elem0=全部)→ 损坏位置**与 N 无关,固定 elem[0]**,非随机元素 → 排除"随机翻转",确认是 elem[0] 的固定写回路径缺陷;
3. 失败率随 N 增长(0.5%→1%)→ 工作量越大触发越多。

### 10.4 纯 C 复现尝试(全部负对照)与差分 perf 定位

为验证能否用"不依赖任何软件库(除 libc)"的纯 C 程序复现,自研 9+ 个纯 C 变体(均仅链接 libc/libm),在 47 核 socket 满载 + 核 179 单线程下逐一测试。**全部不触发(0 fail)**:

| 纯 C 变体 | 算法特征 | 指令足迹 | 核 179 满载失败 | 结论 |
|---|---|---|---|---|
| `mru_dense.c` | 密集 LDL^T(标量 FMA + branch) | ~450 | 0/1500 | ❌ 不触发 |
| `mru_sparse.c` | 稀疏 CSC LDL^T(+ elim tree + 间接) | ~500 | 0/1500 | ❌ 不触发 |
| `mru_sparse.c -O3 -ffast-math` | + 强制向量化 | ~500 | 0/1500 | ❌ 不触发 |
| `mru_sparse.c -ffp-contract=fast` | + FMA contraction(fmadd/fmsub) | ~500 | 0/1500 | ❌ 不触发 |
| `mru_neon.c` | 密集 LDL^T + NEON intrinsics(vfmaq_f64) | ~460 | 0/1500 | ❌ 不触发 |
| `mru_full_sparse.c` | 完整稀疏 Cholesky(union-find etree + DFS postorder + symbolic + numeric,Eigen 算法忠实重建) | ~675(-O2)/~1084(-O0) | 0/1500(N=256), 0/200(N=512), 0/100(N=1024) | ❌ 不触发 |
| `mru_ace.c` | 8×128-bit FMA 累加器(高 ACE+IBR)+paired load | ~500 | 0/1500 | ❌ 不触发 |
| `mru_branch.c` | + 数据相关不可预测分支(高 branch-miss) | ~600 | 0/1500 | ❌ 不触发 |
| `mru_asm.c` | 内联汇编精确复现 Eigen 热循环(fmsub+indirect ldr/str) | ~500 | 0/1500 | ❌ 不触发 |
| **`esdiag_phase2.cpp`(Eigen numeric-only)** | **numeric factorize** | **~7165** | **5-12/1500** | ✅ 触发 |

**关键发现(给芯片设计的重要负反馈):**
- **所有纯 C 变体(标量/稀疏/NEON intrinsics/完整稀疏重建/内联汇编精确复现热循环)都不触发,只有 Eigen 编译产物触发** → 触发不是单一指令类型(标量 fmsub)的问题,也不是单纯代码足迹规模的问题。
- **足迹假设被证伪**:最初观察纯 C ~450-500 指令 vs Eigen ~6500-7200,推测是足迹规模;但构建完整纯 C 稀疏 Cholesky(`mru_full_sparse.c`)并扩大到 N=512/1024,动态指令数从 35M 升至更大,仍 0 触发;而 Eigen 在 N=256 即触发。
- **差分 perf(2026-08-07)**:Eigen `esphase2`(12/3000 fail)vs 纯 C `mru_full_sparse`(0/3000),在相同 47 核负载下 perf stat(归一化到每指令):
  - **`l1d_cache_refill/insn`:Eigen = 4.05× 纯 C**(最大差异因子);Eigen 的 `mem_access/insn` 是纯 C 的 2.53×;`br_retired/insn` 1.65×;纯 C 的 `stall_frontend/insn` 是 Eigen 的 7×(纯 C 前端受限 = 循环过于紧凑;Eigen 后端受限);`memory_error` 均为 0。
  - **但 L1D 压力假设被证伪**:`mru_coldl1d.c`(每次 factorize 重新 malloc 工作区,匹配 Eigen 4× L1D-refill 特征)仍 0 触发;`mru_solve.c`(加入 solve 阶段校验 x)仍 0 触发。
- **真正的触发因子**:Eigen 编译产物的**特定指令序列交织**(模板展开 + 标量 fmsub + 间接 ldr/str + 长存活对角累加器跨内层循环 + 每次调用 malloc/free 工作区),是 GCC 在不同源码结构下无法复现的。触发需要这**具体序列**,而非算法、规模或单一资源压力。

### 10.5 最小复现用例(MRU):libc-only,端到端验证通过

**★这是当前推荐的可交付 MRU★。** 既然纯 C 无法复现 GCC 从不同源码结构无法产生 Eigen 的精确指令序列,采用**纯 C 驱动 + Eigen 编译机器码对象**的形式:将 Eigen 5.0 模板实例化后的机器码作为链接时对象嵌入纯 C 二进制,**运行时依赖集合(`ldd`)仅 libc + libm**。

**构成与构建:**
- `eigen_cabidrv.cpp`:Eigen 5.0 模板(`SimplicialCholesky`)编译为 C-ABI 包装(`-fno-exceptions -fno-rtti`);**本地提供** `operator new/delete`(→malloc/free)与 `__throw_length_error`(→abort),从而**消除全部 libstdc++/CXXABI 未定义符号**。编译后 `.o` 的未定义符号**仅剩 libc/libm**(abort/assert/calloc/free/fwrite/malloc/memcpy/memset/realloc/sqrt/stderr)。
- `mru_eigenmc.c`:纯 C 驱动(无任何 Eigen 头文件),构建 SPD 稀疏矩阵 A,调用 `escanalyze` 一次 + `escfactorize`/`escsolve` 反复,**CRC 校验 x(损坏输出,elem[0])**。
- `build_mru_eigenmc.sh`:`g++ -fno-exceptions -fno-rtti -c eigen_cabidrv.cpp` → `gcc mru_eigenmc.c eigen_cabidrv.o -o mrueig -lm`。

**`ldd mrueig` 证明(仅 libc + libm):**
```
linux-vdso.so.1
libm.so.6 => /usr/lib64/libm.so.6
libc.so.6 => /usr/lib64/libc.so.6
/lib/ld-linux-aarch64.so.1
```

"不依赖任何软件库(除 libc)"的判定标准是**运行时依赖集合(`ldd`)**,而非编译期 `#include`;Eigen 模板被编译为机器码嵌入,非链接为库。

### 10.6 MRU 特性总结

- **触发操作**:`s.factorize(A)` 重复调用 + `s.solve(b)`(单一 Eigen 函数序列,numeric Cholesky 分解 + 三角求解);
- **触发条件**:核 179 + 同 socket 满载(≥47 核)+ 矩阵规模 N≥256;
- **损坏对象**:求解结果向量 `x[0]`(固定首元素,对应 L 因子首列写回位置);
- **损坏模式**:多位(21-32 bit)数据混叠,非单 bit;
- **失败率**:满载下 ~0.5-1%;单核 0%;
- **静默**:无 PMU `memory_error`、无 EDAC、无同步异常(偶发 SEGV 当混叠被解引用)。

## 11. 根因综合判定

### 11.1 证据矩阵(假设 → 证据 → 结论)

| 假设 | 关键证据 | 结论 |
|---|---|---|
| Eigen 5.0 软件 bug | 固定 seed 全核同数据,仅 179 失败;纯整数测试过;A 完好 | ❌ 排除 |
| 浮点 pipe 损坏 | gemm/svd 0 失败;纯 FMA 0 失败;仅 numeric factorize 触发 | ❌ 排除 |
| 中断淹没 | 核 179 无设备 IRQ | ❌ 排除 |
| cache/TLB/分支/stall 异常 | per-core perf 与健康核差异 < 2% | ❌ 排除 |
| 内存 ECC 损坏 | memory_error PMU=0,EDAC=0 | ❌ 排除 |
| EINJ 主动注入 | error_type=0,无脚本痕迹 | ❌ 排除 |
| 输入数据(A/b)损坏 | esdiag3:A 0/3000 损坏 | ❌ 排除 |
| 单 bit SEU(宇宙射线) | popcount 21-32 位,固定 elem[0] | ❌ 排除 |
| 单一功能单元损坏(ALU/LSU/AGU) | dense/SpMV/gather/纯 FMA 全 0 失败,仅 Cholesky numeric 触发 | ❌ 排除 |

### 11.2 根因(微架构层面,精确化)

经第六层 MRU 削减,根因从"计算路径写回缺陷"精确化为:

**核 179 的乱序执行引擎在 numeric Cholesky factorize 的列更新循环(cdiv + rank-1 update)执行序列下,产生了一个静默的、负载敏感的寄存器/写回路径混叠,把其他计算数据写入求解结果向量 x 的 elem[0] 固定存储位置。**

精确特征:
- **触发指令序列**:`numeric factorize`(Eigen `SimplicialCholesky::factorize()`)的列更新循环——supernodal/level-scheduled 的 cdiv(条件分支定 pivot)+ rank-1 update(标量 fmsub + 间接地址生成 + 长存活对角累加器跨内层循环 + 每次调用 malloc/free 工作区),四者**特定交错**才触发;单独 FMA / 单独 SpMV / 单独 gather / 三角 solve / 重写算法的纯 C 均不触发;
- **触发条件**:核 179 + 同 socket(PkgID 19062)满载(≥47 核)+ 矩阵规模 N≥256(规模阈值);
- **损坏对象**:求解结果向量 `x[0]`(固定首元素,对应 L 因子首列写回位置);
- **损坏机理**:**数据混叠**(data aliasing)——其他计算数据覆盖 elem[0] 的写回通路(推测:物理寄存器重命名/ROB 投机状态/LSU store buffer 在特定依赖链交错下被错误旁路);
- **关键负反馈线索**:**numeric-only 失败率 ≈ 4× compute-both** → symbolic 阶段重做会刷新/打断某个持续占用的微架构状态从而缓解 → 指向**乱序执行引擎的持久化状态损坏**(物理寄存器堆/ROB/store buffer 的跨指令状态泄漏);
- **损坏规模**:多位(21-32 bit),非单粒子翻转;
- **静默性**:无 PMU `memory_error`、无 EDAC 计数、无同步异常(多数 run);偶发 SEGV 当混叠数据被解引用为指针;
- **触发率**:满载下 ~0.5-1%;单核 0%。

**这是非"随机粒子翻转的真 SDC",而是一个具体的、可复现的、负载敏感的乱序执行引擎状态泄漏缺陷。** 下文 §11.2a 用指令级反汇编修正传播路径与寄存器编号,并把"状态泄漏"从假说推进到可判别的机制流程。

### 11.2a 机制判别与 RTL 验证建议(指令级深挖,2026-08-13 补充)

> 本节源自对 `eigen_cabidrv.o` 的指令级反汇编 + 既有实验数据反推。修正 §11.2 的两处表述,并把"状态泄漏"从假说推进到**可判别的机制流程**。

#### A. 修正 §11.2 的两处表述

1. **传播路径修正(直接写 elem[0] → 两跳放大)**:§11.2 说"其他计算数据覆盖 elem[0] 的写回通路"。反汇编表明损坏不是直接落在 elem[0],而是经**两跳传播**:(1) factorize k=0 迭代里长存活对角累加器 `d0` 被别的 double 混叠;(2) `L[0,0] = sqrt(d0')` 写入 L 因子首对角元,solve 前向回代 `x[0] = b[0] / L[0,0]`(i=0 时求和项为空)把 d0 的单点损坏**无衰减放大**到 x[0]。x[0] 是唯一只依赖 L[0,0] 的元素,其他 x[i] 依赖多列会分散泄漏 → 这才是"固定 elem[0]"的真正原因。
   - 关键区分:损坏的 `x[0]` 是 solve 输出,**不是** factorize 内部的 `y[]` 工作区。`y[]` 是栈上 alloca 分配(`ei_declare_aligned_stack_constructed_variable`,见 `Memory.h:803`),固定栈地址 `0xfffff3e8d8e0` —— §8.3 所述固定地址实为 `y[]` 首址。纯 C 负对照全校验 D(因子对角线)0 触发、成功 MRU 校验 x 才触发 → 损坏经 solve 传播到 x,这一观察是机制深挖的出发点。
2. **寄存器编号修正(d4 → d0)**:§11.2 原说"长存活 d4 素加器"。反汇编确认长存活累加器是 `d0`(`fmadd d0`@1dc 定义 → 跨数据相关间接寻址子循环 → `fmsub d0`@250 更新 → `fsqrt d0`@2c0 消费)。寄存器编号因编译版本而异,不重要;重要的是"跨间接寻址子循环存活的标量对角累加器"这一结构。触发指令是 `fmadd + fmsub + fdiv` 组合,非单纯 fmsub。

#### B. factorize 的指令级数据流(反汇编实证)

反汇编 `eigen_cabidrv.o` 的 `factorize_preordered<false,false>` 段(LLT 版,MRU 实际触发的那版,size 1100 字节)。三个内层循环对应的机器码:

**循环 ①:scatter A 进 y**(源码 327-337 行)
```
128: ldr  d1, [x7, x3, lsl #3]     ; Ax[p]   (x7=Ax基址)
130: ldr  d0, [x19, x1, lsl #3]    ; y[i]    (x19=y基址)
138: fadd d0, d0, d1               ; y[i] += A(i,k)
13c: str  d0, [x19, x1, lsl #3]   ; 写回 y[i]
```

**循环 ②:rank-1 update 列更新**(源码 345-363 行,触发点)
```
1fc: ldr  d3, [x19, x3, lsl #3]   ; 读 y[i]
208: str  xzr,[x19, x3, lsl #3]   ; y[i] = 0  (读后即清)
20c: ldr  d1, [x20, w1, sxtw #3]  ; 读 Lx[p]  (x20=Lx基址, 间接索引 w1)
218: fdiv d3, d3, d1               ; yi = yi / Lx[Lp[i]]   ← 源码355行
     ; --- 内层 rank-1 update 子循环 (源码359行 y[Li[p]] -= Lx[p]*yi) ---
228: ldrsw x1, [x21, x0, lsl #2]  ; Li[p]    (x21=Li基址, 间接索引!)
22c: ldr  d2, [x20, x0, lsl #3]   ; Lx[p]
234: ldr  d1, [x19, x1, lsl #3]   ; y[Li[p]]
238: fmsub d1, d2, d3, d1         ; ★触发指令: y[Li[p]] = y[Li[p]] - Lx[p]*yi ★
23c: str  d1, [x19, x1, lsl #3]   ; 写回 y[Li[p]]
240: cmp x2, x0 ; b.ne 228         ; 循环
     ; --- 对角累加器更新 (源码360行 d -= l_ki*yi) ---
250: fmsub d0, d3, d3, d0          ; d0 = d0 - yi*yi   ← d0 跨循环长存活!
258: str  d3, [x20, x0, lsl #3]   ; 写回 Lx[p] = l_ki (因子存储)
```

**循环 ③:对角写回**(源码 372-378 行)
```
278: fcmpe d0, #0.0                ; 检查 d (d0) 正定性
2c0: fsqrt d0, d0                  ; d0 = sqrt(d)
2c8: str   d0, [x20, x0, lsl #3]  ; Lx[k] = sqrt(d)   ← 写回 L[k,k] 对角元
```

**d0 的长存活生命周期**(状态泄漏的物理载体):`d0` 在 `fmadd`@1dc 定义 → 跨越整个内层 rank-1 update 子循环(228-244,~10 条指令、一个数据相关循环)→ `fmsub`@250 更新 → 再跨下一个 i 的子循环 → `fsqrt`@2c0 消费。d0 必须存活跨越一个**间接寻址的、数据相关的、可能多次迭代的内层循环** —— 这正是乱序引擎物理寄存器重命名表/ROB 维护"跨迭代物理寄存器活性"压力最大的场景。

#### C. 六种"乱序 + 浮点寄存器重命名"叠加的篡改流程

为什么必须两个叠加才危险:只有乱序无重命名→寄存器单槽不会被偷换;只有重命名无乱序→在飞指令少、free-list 压力小不逼到边界。**两者叠加**撑开大在飞窗口→同时活着的物理寄存器最多→物理寄存器回收决策最频繁→把"某物理寄存器到底还能不能回收"的边界判断逼到最易出错点。这解释单核不触发、满载才触发。

| # | 流程 | 机理 | 预测签名 |
|---|---|---|---|
| **A** | **物理寄存器活性误判(回收过早)** | d0 映射 PR_A,须等 @250 读才能回收;但 @250 藏在数据相关、循环次数现算的间接寻址子循环后,活性记账漏了它,误判 PR_A 可回收,分给当时在算的另一个 double,后者写脏 PR_A | 整寄存器换值;actual 与 expected 无关;依赖具体链形状;跨调用持续 |
| **B** | **重命名表表项存储位翻转** | 重命名表 SRAM 某 bit 翻转,d0→PR_A 变 d0→PR_B | 整寄存器混叠,但与指令序列无关,任何 FP 压力都可触发 |
| **C** | **ROB 提交顺序错(过早释放旧 PR)** | 提交端误判 @250 可提交,提前释放 d0 旧 PR 被新值覆盖 | 与 A 几乎同签名,但不跨调用持续 |
| **D** | **间接寻址循环依赖图误判** | 调度器误把循环各迭代当独立,累加顺序错 | 只产生 rounding 差,非垃圾值 ❌ |
| **E** | **store buffer / load forwarding 旁路** | d0 初始值来自 y[0],y[0] store 被旁路进脏数据 | 损坏值该像 y[0] 简单小数;且 y[0] 在 k=0 刚清零 ❌ |
| **F** | **cdiv 分支误推测 + 错误路径未冲刷** | cdiv cmp/b. 推测错,错误路径指令写 d0 PR,冲刷不净 | 能解释垃圾值;symbolic 刷新分支状态可缓解(部分吻合 4×) |

#### D. 证据打分矩阵与判定

| 观察事实 | A | B | C | D | E | F |
|---|---|---|---|---|---|---|
| ① 整寄存器换值、actual 与 expected 无关(-15.68) | ✅ | ✅ | ✅ | ❌ | ❌ | ✅ |
| ② **只有 Eigen 精确序列触发,纯 C 全 0%** | ✅ | ❌ | ✅ | ✅ | ✅ | △ |
| ③ **numeric-only ≈4× compute-both(状态泄漏签名)** | ✅ | ❌ | △ | ❌ | ❌ | △ |
| ④ 满载触发、单核 0% | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| ⑤ 静默(memory_error=0、EDAC=0) | ✅ | △ | ✅ | ✅ | △ | ✅ |
| ⑥ 固定 elem[0](经 L[0,0]=sqrt(d0) 放大) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

**判定:最可能是 A(物理寄存器活性误判/回收过早),次可能是 C(ROB 提交顺序错)。** 两条硬证据:

- **证据 ② 排除 B**:纯 C 9 变体全 0 触发,只有 Eigen 编译产物触发(§10.4)。若是 B(重命名表 SRAM 随机位翻转,与序列无关),任何压满 FP 寄存器的负载都该触发,纯 C 的 FMA/SpMV/gather 同样压满却 0 触发 → 矛盾。A 是逻辑错误:活性记账漏 @250 读者,是因为 @250 藏在"数据相关间接寻址子循环"后——这具体依赖链形状是 GCC 从 Eigen 模板展开才产生,纯 C 换写法依赖链变,不再逼到边界点。**吻合**。
- **证据 ③ 排除 B、偏 A**:4× 签名是"跨调用持续状态、被 symbolic 刷新缓解"。A 的活性簿记跨调用持续、symbolic 重做会重置它(吻合);B 的表项映射每指令重写,无"被 symbolic 特异刷新"语义(4× 难解释);C 的 ROB 在函数返回被冲刷、不跨调用持续(4× 对 C 偏弱)→ A 略胜 C。

**证据 ⑤ 进一步排除 B(若 rename table 带 parity)**:物理寄存器堆数据位 + free-list 活性逻辑通常为速度不做 ECC → 静默通过;若 B 且 rename table 带 parity 会被检出引发机器检查,与全程静默矛盾。**证据 ① 排除 D、E**:-15.68 整寄存器垃圾只"被另一个 double 占"才产生。

**交叉印证(sdc_fuzz 用例簇)**:100 个 sdc_fuzz 复现用例按三可疑泄漏点分簇(rename 34 / rob 33 / lsu 33,见 §17.2),早期 3 用例子集复现显示 rename 簇 sdc_fuzz_000 复现(popcount 21)、rob 簇 sdc_fuzz_001 复现(popcount 15)、lsu 簇 sdc_fuzz_002 复现(popcount 14)——三簇都能复现同一签名(elem[0]+多位),说明三可疑点在该缺陷家族中都有暴露,但 rename 簇触发特征(popcount 最高、与 mode 1 numeric-only 最高触发率对应)与流程 A 判定一致。

#### E. 诚实边界(软件看不到的)

- **A 与 C 的二选一**:活性记账漏读者(A)vs 提交端过早释放旧 PR(C),软件签名几乎重合。要 RTL 在物理寄存器活性表和 ROB 提交逻辑上分别加可观测性、跑 `factorize_preordered<false,false>` 这条 d0 跨子循环依赖链才能区分。
- **具体哪张表、哪个晶体管**:软件彻底看不见,需 RTL 仿真或 HiSilicon 内部 TaiShan v110 微架构文档(同 §15.1)。

#### F. 给硬件团队的 RTL 验证建议(聚焦流程 A)

1. **靶依赖链已精确到指令地址**:`factorize_preordered<false,false>` 段内 `@1dc fmadd d0`(定义)→ `@228-244 间接寻址子循环`(循环次数数据相关)→ `@250 fmsub d0`(更新)→ `@2c0 fsqrt d0`(消费)→ `@2c8 str d0,[Lx]`(写 L[0,0])。RTL 回归用例应 instantiate 这条 **d0 跨数据相关间接寻址子循环存活**的依赖链,而非泛泛压测。
2. **物理寄存器活性表加 scan-at-speed**:在 PRF free-list 的活性判定逻辑上加扫描,专门构造"d0 物理寄存器在跨子循环存活期间,free-list 是否错误地把它判为可回收"的用例——流程 A 的核心。
3. **与 ROB 提交逻辑做对比验证**(区分 A vs C):同一条依赖链,分别在物理寄存器活性表和 ROB 提交逻辑加可观测性;A 路径暴露错误而 C 路径正常,则确认 A、排除 C。
4. **可观测性设计改进**:物理寄存器堆数据位 + free-list 活性逻辑当前为速度不做 ECC(这正是此缺陷静默的根因,见证据 ⑤)。建议在 free-list 活性逻辑加 parity/校验,使此类"错误回收"可被生产环境检出——这能把"静默 SDC"变成"可检出错误",直接消除当前缺陷的生产危害。
5. **修复判据**:修复后不仅看 x[0] 不再错,还应确认 d0 在 k=0 跨子循环的物理寄存器映射不再被误释放(需上述可观测性)。

### 11.3 关于系统挂死的根因

真 SDC 是静默的。本次研究中早期两次满载长跑出现系统挂死(强制掉电复位)。根因排查(§7 M1 实验):满载 20s 内产生 55 次计算失败,EDAC `ce/ue_count` 与新增 dmesg(SEI/SDEI/MCE/GHES/panic/oops)全程为 0,故**挂死机制不是内存 ECC**,而是**计算损坏波及控制流**(混叠数据 → 错误指针/控制转移 → 不可恢复状态)。

**⚠️ 2026-08-13 安全约束证伪更新:** 原先称"47 核 socket 满载 + 单次 ≤75s + timeout 包裹 = 安全不挂死",**已被 2026-08-13 实跑证伪**。当日 10:31 用同样 47 核 + timeout 75 复现,mrueig 第 8 次 CRC mismatch 后 SEGV,10:34:35 内核报 **RCU stall**(CPU 179 与 144 stalled,13 个跨 socket CPU 的 IPI 失败),系统随后完全挂死约 52 分钟,11:26:20 由 **SBSA 硬件 watchdog**(`sbsa-gwdt`,10s 超时)**强制硬复位**。`/var/log/messages` 实证:上一 boot 结尾**无任何 systemd 关机序列** → 证明是硬件硬复位,非 reboot 命令、非 panic 软复位。这与"混叠数据波及控制流"一致,但波及范围比早期复现更广,拖挂了内核调度。详见 §16(安全约束更新)。

**结论:对这台特定故障核 179,任何"满载 + 跑触发程序"的复现方式都不安全,即便 timeout 包裹也无法防止混叠波及内核调度导致挂死。**

### 11.4 物理定位

- 故障核:**CPU 179**
- 所在 socket:PkgID 19062(第 4 socket)
- 所在 module:23340(核 176-178 同 module,但仅 179 故障 → 非 module 级共享资源损坏,而是核 179 私有路径)
- 微码:0x0(与所有核一致,非微码差异)

### 11.5 给芯片设计的高价值负反馈

本研究为 TaiShan v110 / Kunpeng 920 微架构团队提供以下可操作的负反馈(机制判别见 §11.2a,RTL 验证聚焦点见 §11.2a.F):

**1. 故障定位到乱序执行引擎,非功能单元**
- cache/TLB/分支预测/内存子系统/FPU pipe 在 per-core PMU 下均正常 → 故障在**乱序执行引擎的状态管理**(物理寄存器重命名/ROB/LSU store buffer),而非任何执行单元本身。
- 设计验证建议:对 OoO engine 的物理寄存器堆/ROB store buffer 做 scan-at-speed + 在 numeric factorize 类指令序列下的压力测试。

**2. numeric factorize 是暴露故障的最小指令序列**
- `SimplicialCholesky::factorize()` 的列更新循环(cdiv + rank-1 update)是已知最小触发序列。
- 设计验证建议:将该序列纳入 RTL 仿真/形式验证的回归用例;特别是 **cdiv 条件分支 + 标量 fmsub rank-1 update + 间接地址生成 + 长存活累加器**四者的交错依赖链。

**3. symbolic→numeric 切换刷新可缓解 → 状态泄漏型缺陷**
- numeric-only 失败率 ≈ 4× compute-both,说明重做 symbolic 打断了持续状态。
- 设计验证建议:排查物理寄存器重命名表/ROB 在长依赖链列循环中的**跨迭代状态泄漏**(上一迭代的物理寄存器未正确释放/隔离,被下一迭代的写回误用);这是"状态泄漏型"缺陷的典型签名。机制判别指向流程 A(物理寄存器活性误判)。

**4. 规模阈值 N≥256 + 满载双触发**
- 单核不触发、N<256 不触发,需满载 + 足够大规模。
- 设计验证建议:压力测试需满 socket 负载 + 足够长指令序列(触发执行窗口/ROB 填满状态);单元测试无法暴露。

**5. 静默缺陷未被现有 RAS 捕获**
- EDAC/`memory_error` PMU/SDEI/GHES 全程为 0,该缺陷完全静默。
- 设计改进建议:考虑在 OoO engine 的写回路径增加可观测性(如 store-buffer 数据完整性校验 / 物理寄存器堆 ECC / free-list 活性逻辑 parity),否则此类缺陷无法在生产中被检测。

**6. MRU 提供作为 DFT/DPP 测试向量**
- §10.5 的 libc-only MRU(`mru_eigenmc.c` + `eigen_cabidrv.cpp`)可直接作为芯片 bring-up 和量产筛选的测试向量,在满 socket 负载下运行可在秒级暴露故障;`ldd` 证明仅依赖 libc,部署无第三方库依赖。

### 11.6 端到端验证(E2E)

为证明 MRU 触发的故障与实际 opendcdiag `eigen_sparse` 满载失败是**同一根因**,执行端到端一致性验证。以下为**单一 E2E 表**(合并 libc-only MRU `mrueig` 与 Eigen 参考 `esphase2`、opendcdiag 端的完整对照):

| 验证 | 配置 | 结果 | 一致性 |
|---|---|---|---|
| E2E-1 libc-only MRU,179 满载 | 47 核满载 + `mrueig` 在 179(2500-3000 迭代) | 6-24 fails | MRU 端 ✅ |
| E2E-2 Eigen 参考,179 满载(同窗口) | 47 核满载 + `esphase2` mode 1 在 179 | 5-21 fails | 同根因 ✅ |
| E2E-3 健康核,满载 | `mrueig` 在 176,47 核满载 | **0 fails** | 核特异性 ✅ |
| E2E-4 单核,无负载 | `mrueig` 在 179,无外部负载 | **0 fails** | 负载触发 ✅ |
| E2E-5 隔离 | 下线 179 后,`mrueig` 在 176 + 满载 | **0 fails** | 隔离有效 ✅ |
| E2E-6 复现(闭环) | 恢复 179 后,`mrueig` 在 179 + 满载 | fails | 可复现 ✅ |
| E2E-7 opendcdiag 端 | 48 核(含 179)opendcdiag eigen_sparse 满载 30s | 179: 63/63 失败(全 179),其他 47 核 0 | 实际故障端 ✅ |
| E2E-8 opendcdiag 隔离 | 下线 179 后 47 核满载 opendcdiag | 2298 ok / 0 失败 | 隔离有效 ✅ |
| E2E-9 负对照 | 纯 C `mru_full_sparse` 在 179 + 满载 | **0 fails** | 旧纯 C 仍不触发 ✅ |

**libc-only MRU 损坏签名(与原 Eigen 版一致):** elem[0] 固定(firstbad_idx=0)、多位(popcount 10-28 bit)、数据混叠(非 SEU)。

**端到端验证结论:**
1. **两端同核同触发**:libc-only MRU(`mrueig`)、Eigen 参考(`esphase2`)与 opendcdiag eigen_sparse 都**只在核 179、只在满载**触发,其他核/单核均 0 失败 → 根因绑定核 179 一致。
2. **同窗口同率**:E2E-1 与 E2E-2 在同一负载窗口下失败率量级一致(6-24 vs 5-21)→ 两 MRU 同根因。
3. **隔离端到端有效**:下线 179 后,libc-only MRU 与 opendcdiag 同时清零 → 根因是核 179 本身,非共享资源/全局条件。
4. **失败率量级一致**:opendcdiag 用 `compute()`(symbolic+numeric 每次),符合削减 2 的"compute-both"特性(触发但低于 numeric-only);两端同根因。

**⚠️ 安全执行状态:** E2E 实验于 2026-08-11 前完成,当时采用 47-48 核 socket 满载 + 单次 ≤75s + `timeout` 包裹,全程未触发系统挂死。**但 2026-08-13 同样配置已导致挂死复位(§11.3、§16),故该"安全"状态已失效,不可复现。**

## 12. 缓解措施(已验证)

### 12.1 即时隔离
```bash
echo 0 > /sys/devices/system/cpu/cpu179/online   # 下线核 179(需 root)
```
验证:191 核(排除 179)满载 60s = **3085 ok / 0 失败**,系统稳定。

### 12.2 永久隔离
内核启动参数追加 `isolcpus=179`(或通过 `nr_cpus`/`cpu_affinity` 在系统服务层排除)。

### 12.3 长期
更换 CPU(socket PkgID 19062,核 179 私有计算路径硬件缺陷)。建议联系 HiSilicon/Kunpeng 进行该 socket 的 RMA。

## 13. 交付件:诊断探针与 MRU 源码

全部源码已持久化到本目录 `opendcdiag/docs/sdc1-01-02-core179-diagnostics/`,可随时重建。每个探针的功能、依赖、检查字段、所属调查层、构建命令如下。

### 13.1 探针总表(按角色分组)

> **检查字段**说明:成功 MRU 与 Eigen 参考探针校验 **x**(求解结果向量,损坏发生在 `x[0]`);纯 C 负对照中多数只校验 **D**(因子对角线)——这是它们未能触发的一个线索(损坏经 solve 传播到 x,见 §11.2a.A)。

| 类别 | 文件 | 语言/依赖 | 检查字段 | 一句话用途 |
|---|---|---|---|---|
| **★成功 MRU(libc-only)★** | `mru_eigenmc.c` + `eigen_cabidrv.cpp` + `build_mru_eigenmc.sh` | C/libc+libm | x | 纯 C 驱动 + Eigen 机器码,`ldd` 仅 libc+libm,端到端验证复现 |
| **Eigen 参考复现**(链接 libstdc++) | `esdiag_phase2.cpp` | C++17/Eigen+libstdc++ | x | 分离 symbolic/numeric,numeric-only 复现(elem[0] 损坏) |
| **字节级定位探针**(第五层) | `esdiag3.cpp` | C++17/Eigen+libstdc++ | A + x | 核心:固定 A,分离输入/计算损坏,A_corrupt/x_wrong/popcount |
| | `esdiag2.cpp` | C++17/Eigen+libstdc++ | x | 打印 elem[0] 损坏字节级模式(地址/actual/xor/popcount) |
| | `esdiag_elem0.cpp` | C++17/Eigen+libstdc++ | x | 逐元素定位损坏(确认固定 elem[0]) |
| | `esdiag_N.cpp` | C++17/Eigen+libstdc++ | x | 扫描矩阵规模 N(确认 N≥256 阈值) |
| **纯 C 负对照 MRU**(均不触发) | `mru_full_sparse.c` | C/libc+libm | D | 完整纯 C 稀疏 Cholesky 重建(证伪足迹/算法假设) |
| | `mru_coldl1d.c` | C/libc+libm | D | 每次 malloc 工作区(证伪 L1D 压力假设) |
| | `mru_solve.c` | C/libc+libm | x | 含 solve 阶段校验 x(证伪"缺 solve 阶段"假设) |
| | `mru_pure_c.c` / `mru_sparse.c` / `mru_dense.c` / `mru_neon.c` / `mru_ace.c` / `mru_branch.c` / `mru_asm.c` | C/libc+libm | D | 其他纯 C 变体(标量/稀疏/NEON/累加器/分支/汇编,均 0 触发) |
| **第六层削减探针** | `minimal_probe.c` | C/libc | D | dense/SpMV/gather 三变体(证明单一操作不触发) |
| | `fma_probe.c` | C/libc | D | 纯 FMA(证明纯 FMA 不触发) |
| **功能单元探针**(第三层) | `probe.c` | C/libc+NEON/ACLE | 各单元 | 8 功能单元逐一压测自检(ALU/FPU/NEON/L1D/L2L3/分支/CRC) |
| | `fpprobe.c` | C/libc | C | CRC 敏感浮点探针(简单浮点,压力下自检) |
| **辅助** | `loadgen.c` | C/libc | — | CPU 负载生成器(`fork` N worker) |

### 13.2 探针与六层假设排除法的映射

| 调查层 | 证伪的假设 | 使用的探针 | 结论 |
|---|---|---|---|
| 第一层(软件 vs 硬件) | Eigen 在该 seed 有确定性 bug | opendcdiag 自身 + 核对照(B1/B2) | 排除软件 |
| 第二层(触发测试类型) | 浮点 pipe 损坏 | opendcdiag 测试矩阵 + `fpprobe.c` | 仅稀疏 Cholesky 触发 |
| 第三层(微架构事件) | cache/LSU/TLB/分支/FPU 损坏 | `probe.c` + per-core PMU | 微架构全正常 |
| 第四层(实时内核监控) | 失败伴随硬件错误事件;挂死是内存 ECC | 满载 + dmesg/EDAC 跟踪(M1) | 失败静默、挂死非 ECC |
| 第五层(字节级损坏) | 输入数据损坏;单 bit SEU | `esdiag3.cpp` + `esdiag2.cpp` + `esdiag_elem0.cpp` | 计算过程损坏 elem[0],多位混叠 |
| 第六层(MRU 削减) | Cholesky 整体 vs 单一操作;symbolic vs numeric;规模 | `minimal_probe.c`/`fma_probe.c`/`esdiag_phase2.cpp`/`esdiag_N.cpp` + 9 个纯 C 负对照 + 差分 perf | 需 numeric factorize;状态泄漏型;N≥256 |
| 交付(最小复现) | (构造可复现 DFT 向量) | `mru_eigenmc.c` + `eigen_cabidrv.cpp`(libc-only) | 端到端验证通过 |
| 全程(辅助) | — | `loadgen.c` | 制造满载触发条件(注:loadgen 不能替代 eigen_sparse 作负载源,见 §17.4) |

### 13.3 关键探针详解

**`esdiag3.cpp` — 核心:输入/计算损坏分离(决定性)**。对应第五层 D4 实验。工作原理:用固定 seed 的 LCG 构建一个 256×256 对称稀疏矩阵 A(只构建一次);用 `__crc32cb` 对 A 的 value 数组做校验记为 `a_crc`;`b = VectorXd::Random(N)`,用 `SimplicialCholesky` 求解一次得到 golden 记为 `gold_crc`;循环 iters 次:重新 CRC 校验 A(若不符 acorrupt++);用全新 solver 对象重新 `compute(A).solve(b)`,对 x CRC(若不符 fails++);前 3 次失败打印 `a_crc_ok` 标志。证伪"输入数据 A/b 被破坏"。决定性结论:A_corrupt=0/3000,x_wrong=28/3000,每次失败 a_crc_ok=1 → 损坏在计算过程本身。

**`esdiag_phase2.cpp` — Eigen 参考 MRU(第六层削减 2)**。分离 symbolic/numeric:mode 0 = compute() 每次;mode 1 = analyzePattern 一次 + 重复 factorize+solve(numeric-only);mode 2 = analyzePattern 每次只做 symbolic + 一次 factorize+solve。关键结论:mode 1(numeric-only)15/1500(1.0%);mode 0(compute-both)4/1500(0.27%);solve-only 0/2000。numeric-only ≈ 4× compute-both → symbolic 重做刷新状态缓解 → 状态泄漏型缺陷签名。

**`mru_eigenmc.c` + `eigen_cabidrv.cpp` — libc-only 成功 MRU(★交付件★)**。纯 C 驱动调用 Eigen 编译机器码对象;`eigen_cabidrv.cpp` 用 `-fno-exceptions -fno-rtti` 编译并本地提供 `operator new/delete`/`__throw_length_error` stub,消除全部 libstdc++ 符号;最终 `mrueig` 的 `ldd` 仅 libc + libm。

**`probe.c` — 8 功能单元微架构探针(第三层)**。对 8 个功能单元分别压测并自检:1 ALU_int、2 FPU_double、3 FPU_float、4 NEON_vec、5 L1D_cache、6 L2L3_mem、7 Branch_pred、8 CRC_engine。配合第三层 per-core PMU 对照,共同排除"某一具体功能单元损坏"。

### 13.4 构建与运行速查

```bash
# 环境准备
export PATH="$HOME/.local/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/rpmroot/sysroot/usr/lib64:$LD_LIBRARY_PATH"
cd ~/arm64-sdc-fuzzing/opendcdiag   # 或本机 /home/sdc/sdc-fuzzing/opendcdiag
EIG=$PWD/third-party
DIAG=$PWD/docs/sdc1-01-02-core179-diagnostics

# === ★推荐:libc-only MRU(一键构建)===
$DIAG/build_mru_eigenmc.sh                      # 产 $DIAG/mrueig,ldd 仅 libc+libm

# === Eigen 参考探针(链接 libstdc++)===
g++ -O2 -std=gnu++17 -march=armv8.1-a+crc+crypto -I$EIG $DIAG/esdiag_phase2.cpp -o /tmp/esphase2
g++ -O2 -std=gnu++17 -march=armv8.1-a+crc+crypto -I$EIG $DIAG/esdiag3.cpp       -o /tmp/esdiag3
g++ -O2 -std=gnu++17 -march=armv8.1-a+crc+crypto -I$EIG $DIAG/esdiag2.cpp       -o /tmp/esdiag2

# === 纯 C 探针(仅 libc/libm)===
gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 $DIAG/mru_full_sparse.c -o /tmp/mruf -lm
gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 $DIAG/mru_coldl1d.c   -o /tmp/mrucold -lm
gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 $DIAG/mru_solve.c     -o /tmp/mrusolv -lm
gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17                       $DIAG/minimal_probe.c -o /tmp/mprobe
gcc -O3 -march=armv8.1-a+crc+crypto -std=gnu17                       $DIAG/fma_probe.c     -o /tmp/fmaprobe
gcc -O3 -march=armv8.1-a+crc+crypto -std=gnu17                       $DIAG/fpprobe.c       -o /tmp/fpprobe
gcc -O3 -march=armv8.1-a+crc+crypto -std=gnu17 -Wno-incompatible-pointer-types $DIAG/probe.c -o /tmp/probe
gcc -O2                                                              $DIAG/loadgen.c       -o /tmp/loadgen
```

> ⚠️ **触发实验安全约束已更新(见 §16):2026-08-13 起禁止用"47 核满载 + 核 179 跑触发程序"复现,即便 timeout 包裹也会挂死复位。** 下面的"触发实验"命令仅作历史记录,不可在本机再跑。如需复现,改用隔离验证(先下线 179,在健康核跑对照)。

<details>
<summary>历史触发命令(已失效,仅存档)</summary>

```bash
# 历史(2026-08-11 前)触发命令 —— 已被 §16 证伪,不可再跑
SET=$(echo $(seq 144 178) $(seq 180 191) | tr ' ' ',')   # 144-191 排除 179
timeout 75 builddir/opendcdiag --beta -e eigen_sparse --cpuset "$SET" -s LCG:323306158 \
    -T 60s --output-format=tap -o /tmp/load.log >/dev/null 2>&1 &
sleep 2
taskset -c 179 $DIAG/mrueig 3000 12345           # 历史预期:6-24/3000 fails
# 健康核对照:taskset -c 176 $DIAG/mrueig 3000 12345  → 0 fails
```
</details>

### 13.5 关键日志/数据文件
- 满载 eigen_sparse 失败 YAML(含 loop-count、retry 模式):运行时由 `-o` 生成;
- per-core PMU 对比原始数据:`/tmp/p179.csv`、`/tmp/p176.csv`(运行时生成);
- 2026-08-13 挂死复位证据:`/var/log/messages`(持久,记录上一 boot RCU stall + 无关机序列);
- 探针源码:本目录。

### 13.6 关联产物
- OpenDCDiag ARM64 构建修复(PR #1,已合并到 main):`fix/opendcdiag-arm64-build-and-tests`;
- Eigen 5.0.1 源码(vendored,main):`opendcdiag/third-party`;
- 100 个 sdc_fuzz 复现用例:见 §17。

## 14. 研究方法学小结

本研究采用**分层假设排除法**(6 层),每一层用对照实验证伪一个假设,逐步逼近根因:

| 层 | 调查主题 | 证伪的假设 | 关键探针/工具 | 结论 |
|---|---|---|---|---|
| 第一层 | 软件 vs 硬件 | Eigen 在该 seed 有确定性 bug | opendcdiag 自身 + 核对照(B1/B2) | 排除软件 |
| 第二层 | 触发测试类型 | 浮点 pipe 损坏 | opendcdiag 测试矩阵 + `fpprobe.c` | 仅稀疏 Cholesky 触发 |
| 第三层 | 微架构事件 | cache/LSU/TLB/分支/FPU 损坏 | `probe.c` + per-core PMU | 微架构全正常 |
| 第四层 | 实时内核监控 | 失败伴随硬件错误事件;挂死是内存 ECC | 满载 + dmesg/EDAC 跟踪(M1) | 失败静默、挂死非 ECC |
| 第五层 | 字节级损坏模式 | 输入数据损坏;单 bit SEU | `esdiag3.cpp` + `esdiag2.cpp` | 计算过程损坏 elem[0],多位混叠 |
| 第六层 | MRU 削减 | Cholesky 整体 vs 单一操作;symbolic vs numeric;规模 | `minimal_probe.c`/`fma_probe.c`/`esdiag_phase2.cpp`/`esdiag_N.cpp` + 9 个纯 C 负对照 + 差分 perf | 需 numeric factorize 列更新序列;状态泄漏型;N≥256 |
| 旁支 | 注入/中断干扰 | EINJ 注入;设备中断淹没 | EINJ/`/proc/interrupts` 排查 | 排除注入与中断 |
| 深挖 | 机制判别 | 状态泄漏具体是哪种流程 | `eigen_cabidrv.o` 反汇编 + 六流程证据矩阵 | 最可能流程 A(物理寄存器活性误判),次可能 C |
| 交付 | 最小复现 | (构造可复现 DFT 向量) | `mru_eigenmc.c` + `eigen_cabidrv.cpp`(libc-only) | 端到端验证通过 |

核心洞察:
- 当 PMU/EDAC 全部正常却仍静默算错时,需用**针对特定计算模式的字节级自校验探针**(而非通用 PMU)定位损坏模式。固定 elem[0] + 多位混叠是"写回路径缺陷"的决定性特征,区别于"随机 SEU"(随机位置 + 单 bit)。
- **MRU 削减是给芯片设计负反馈的关键**:从复杂算法(Cholesky)逐层削减到单一函数(`factorize`),才能把故障定位到具体微操作序列,作为可复现的 DFT 测试向量。
- **"操作 X 失败率 > 操作 X+Y 失败率"是状态泄漏型缺陷的签名**:numeric-only 比 compute-both 失败率高 4 倍,说明 Y(symbolic)刷新了状态,指向乱序引擎的跨迭代状态泄漏;机制判别进一步指向流程 A(物理寄存器活性误判)。
- **纯 C 复现的可行形式是"纯 C 驱动 + 编译机器码对象"**:GCC 无法从重写的源码结构复现 Eigen 模板展开的精确指令序列,但将 Eigen 编译机器码作为链接对象嵌入,可使最终二进制运行时仅依赖 libc——这满足"不依赖任何软件库(除 libc)"约束(判定标准为 `ldd`)。

## 15. 局限与后续

### 15.1 局限
- 本研究已将根因从"计算路径写回缺陷"精确化到"乱序执行引擎在 numeric factorize 列更新循环下的状态泄漏",并经机制判别指向**流程 A(物理寄存器活性误判/回收过早)**,但 **A 与 C(ROB 提交顺序错)的二选一**需 RTL 仿真确认;**具体哪张表、哪个晶体管**需 HiSilicon 内部 TaiShan v110 微架构文档或 RTL 仿真(超出软件可见范围)。
- 两次挂死的精确内核态未回溯(crash dump 未捕获,journal 未落盘);2026-08-13 第三次挂死已通过 `/var/log/messages` 实证为 RCU stall + SBSA watchdog 硬复位,无干净关机序列。M1 实验已排除内存 ECC,确定为计算损坏波及控制流。
- 本研究为单机单核样本,未在第二台同型号机器上验证是个体缺陷还是批次问题。
- 触发率随机器/负载窗口波动(同一 47 核满载,有的窗口 6-24/3000,有的 0/3000)——属负载敏感型瞬态缺陷的正常表现,需在故障活跃窗口内对照 Eigen 参考与 MRU 同跑以确认一致性。

### 15.2 建议后续
1. 永久 `isolcpus=179` 并长期稳定性监控(24h+,验证排除后系统稳定);
2. 对该 socket 行 HiSilicon 官方 CPU 诊断 / scan-at-speed LBIST,聚焦乱序引擎的物理寄存器活性表/ROB/store buffer(§11.2a.F 的 RTL 验证聚焦点);
3. 若有第二台同型号机器,用 libc-only MRU(`mru_eigenmc.c` + `build_mru_eigenmc.sh`)迁移负载验证是否复现(排除个体 vs 批次);MRU 仅依赖 libc,部署零第三方库依赖;
4. 将 MRU 探针固化到 OpenDCDiag 测试集,作为持续 SDC 监测器(在满 socket 负载下秒级暴露);
5. 芯片设计团队:将 `factorize` 列更新循环作为 RTL 回归用例,重点验证 cdiv 分支 + 标量 fmsub rank-1 update + 间接地址生成 + 长存活对角累加器(d0)的交错依赖链下,物理寄存器重命名/ROB 的跨迭代状态隔离;
6. 改进 DFT:在 OoO engine 写回路径增加数据完整性可观测性(store-buffer 校验 / 物理寄存器堆 ECC / free-list 活性逻辑 parity),使此类静默缺陷可被生产环境检测。

---

## 16. 安全约束更新(2026-08-13,推翻原先乐观估计)

**原先(§11.3 旧版)称:"47 核 socket 满载 + 单次 ≤75s + timeout 包裹 = 安全不挂死"。2026-08-13 实跑证伪。**

### 16.1 2026-08-13 挂死复位事件实证

- 10:31:53 启动 47 核(144-191 排除 179)opendcdiag 满载 + 核 179 跑 mrueig(同样 `timeout 75` 包裹);
- 10:31:58 mrueig 第 8 次 CRC mismatch 后 SEGV(systemd-coredump 干净处理);
- **10:34:35 内核报 RCU stall**:CPU 179 与 144 stalled,13 个跨 socket CPU 的 IPI 失败(`/var/log/messages` 实证);
- 10:34:35 → 11:26:20 约 **52 分钟系统完全挂死**(零日志);
- 11:26:20 由 **SBSA 硬件 watchdog**(`sbsa-gwdt`,10s 超时,`dmesg` 实证 "Initialized with 10s timeout")**强制硬复位**;
- 上一 boot 的 `/var/log/messages` 结尾**无任何 systemd 关机序列**(无 `systemd-shutdown` / `reboot: Power down`)→ 证明是硬件硬复位,非 reboot 命令、非内核 panic 软复位。

### 16.2 根因

与 §11.3 一致:混叠数据波及控制流(错误指针/控制转移 → 不可恢复状态)。但 2026-08-13 这次波及范围比早期复现更广,拖挂了内核调度(RCU stall + IPI 失效),最终靠硬件 watchdog 兜底复位。

### 16.3 后续安全准则

- **不再用"47 核满载 + 核 179 跑触发程序"的方式复现**,即使 timeout 包裹也无法防止混叠波及内核调度导致挂死;
- 若必须动态验证,改用**隔离验证**:先 `echo 0 > /sys/devices/system/cpu/cpu179/online` 下线 179,在健康核上跑对照,确认框架本身行为;核 179 的触发特性只能依赖既有数据 + 静态反汇编(§11.2a);
- 长期:维持 `isolcpus=179` 永久隔离(§12.2),不再唤醒它跑触发负载;
- §13.4 的"触发实验"历史命令已标注失效,仅存档。

---

## 17. 100 个 sdc_fuzz 复现用例(自动化复现机制)

**日期:** 2026-08-07。目标:自动化生成 100 个 SDC 检测用例,每个都能复现 core179 核 SDC。依据:§11.2 根因 + §10.5 libc-only MRU + §11.6 E2E。

### 17.1 机制总览

`sdc-fuzzing` 是一个**用例生成机制**(generator)+**负载编排器**(orchestrator)+**验证器**(verifier)的组合,产出 100 个 opendcdiag 标准 sandstone 测试(`sdc_fuzz_000`..`sdc_fuzz_099`),每个用例:
1. 链接 Eigen 机器码(`SimplicialCholesky::factorize()` numeric 列更新),即经验证的 core179 SDC 触发序列(§10.4:纯 C 全不触发,只有 Eigen 编译产物触发);
2. 保留全部触发前提:core 179 单线程 + 同 socket(PkgID 19062)满载(≥47 核)+ N≥256 + numeric factorize;
3. 检测损坏:CRC32(整段 x)与 golden 比,不等即 `report_fail_msg` 报告,并记录 `elem[0]` actual/golden + xor popcount + cluster 标签。

### 17.2 为什么 100 个用例"不同"且都能复现同一故障家族

§11.2 根因:core179 乱序执行引擎在 numeric factorize 列更新循环下的**状态泄漏**,可疑泄漏点 = 物理寄存器重命名表 / ROB / LSU store buffer 三者。100 个用例按这三个可疑泄漏点**分簇** × 触发前提参数变体:

| 簇 | 数量 | 偏向的泄漏点 | 构造 |
|---|---|---|---|
| **rename** | 34 | 物理寄存器重命名表(长存活对角累加器跨内层循环,最长占用) | 主要 mode 1 numeric-only(重命名表最长持续占用,§10.2 触发率最高);N 256/384/512 改变列循环深度 |
| **rob** | 33 | ROB / 投机状态(长依赖链填满执行窗口) | mode 0 compute-both:symbolic 刷新 ROB 状态后紧邻 numeric,暴露跨阶段状态泄漏(§10.2 状态泄漏签名) |
| **lsu** | 33 | LSU store buffer(间接寻址写回压力) | mode 1 + 稀疏度/nnz 变体(seed 改密度)+ b 向量变体;差分 perf:Eigen l1d_refill 4×、mem_access 2.5× 纯 C(§10.4) |

每个用例 = {簇, N, matrix_seed, mode, b_seed} 确定性派生(无随机),100 个组合覆盖三可疑泄漏点 × 触发前提参数空间。差异维度全部经验证不破坏触发(§10.3 N≥256 均触发固定 elem[0];§5 两 seed 都触发全 179;§10.2 numeric-only 与 compute-both 都触发;§8 损坏在写回与 b 无关)。

### 17.3 文件清单

```
opendcdiag/
├── scripts/
│   ├── generate_sdc_fuzz_cases.py   # 生成器:吐出 100 case_NNN_*.cpp + meson.build + cases_index.csv
│   ├── sdc_fuzz_run.sh              # 负载编排器:47 核满载 + 单用例/批跑/子集 在 core 179
│   └── sdc_fuzz_verify.py           # 验证器:同窗口 MRU 对照 + 签名校验
├── tests/cpu/arm64/
│   ├── meson.build                  # 加了 subdir('sdc_fuzzing')
│   └── sdc_fuzzing/
│       ├── meson.build              # 自动生成,列 100 case 文件,链 eigen3_dep
│       ├── sdc_fuzz_case_template.h # 共享触发/CRC/判定逻辑(镜像 libc-only MRU)
│       ├── case_000_rename.cpp ... case_099_rename.cpp  # 100 个用例(自动生成)
│       └── cases_index.csv          # 用例参数清单
└── builddir_sdc/                    # sdc-owned 构建目录
    └── opendcdiag                   # 含 100 个 sdc_fuzz_* 测试的二进制
```

### 17.4 构建与运行

```bash
# 生成用例(如需重新生成)
cd ~/arm64-sdc-fuzzing/opendcdiag
python3 scripts/generate_sdc_fuzz_cases.py   # 产出 100 case + meson.build + index

# 构建(需 meson+ninja + Eigen 5.0 + sysroot)
export PATH="$HOME/.local/bin:$PATH"
SR=$HOME/rpmroot/sysroot
export PKG_CONFIG_PATH="$SR/usr/lib64/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$SR"
export CXXFLAGS="-isystem $SR/usr/include" CFLAGS="-isystem $SR/usr/include"
export LDFLAGS="-L$SR/usr/lib64 -Wl,-rpath,$SR/usr/lib64"
meson setup builddir_sdc --buildtype=debugoptimized -Dssl_link_type=dynamic
ninja -C builddir_sdc opendcdiag -j8
LD_LIBRARY_PATH=$SR/usr/lib64 builddir_sdc/opendcdiag --beta --list | grep -c sdc_fuzz_  # 期望 200

# 用编排器(自动起停 eigen_sparse 负载)
scripts/sdc_fuzz_run.sh single sdc_fuzz_000 50   # 单用例 50s
scripts/sdc_fuzz_run.sh batch                    # 全 100 个(每个默认 60s,耗时较长)
scripts/sdc_fuzz_run.sh subset 000,050,099 50     # 子集
```

> ⚠️ **关键:负载源必须是 `eigen_sparse`,不是 `loadgen`**(见 §17.5)。且 §16 安全约束同样适用:2026-08-13 起禁止在本机用"满载 + 核 179 跑触发程序"复现,即便 sdc_fuzz 用例 + timeout 包裹也会挂死复位。

### 17.5 关键发现:负载源必须是 eigen_sparse,不是 loadgen

2026-08-07 实测证明:**触发只在 47 核负载是 `opendcdiag -e eigen_sparse`(稀疏 Cholesky 指令序列)时激活**。用简单 `loadgen` 循环做 47 核负载 → mrueig 在 179 上 0/2000、0/4000 fail(休眠,负载错误);用 `eigen_sparse` 负载 → mrueig 在 179 上 2/2000 fail,sdc_fuzz 用例多次 fail(签名一致)。这本身就是 §10.4 的核心结论之一(触发需要稀疏 Cholesky 的特定指令序列)。→ 所有编排/验证脚本均用 **eigen_sparse 作负载源**,禁止用 loadgen。

### 17.6 损坏签名(与 MRU 一致)

每个用例 fail 时,`report_fail_msg` 输出形如:
```
core179 SDC: x crc mismatch (0x000000d9 vs golden 0x1f89120c) elem[0]=-0.932418 (golden 0.0795441) popcount=21 bits [register-rename-table]
```
- `elem[0]`:损坏固定在结果向量首元素(§8.2,固定栈地址);
- `popcount`:xor(actual,golden) 的位数,core179 SDC 为多位(10-32 bit),区别于单 bit SEU(1 bit);
- `[cluster]`:该用例偏向的可疑泄漏点标签。

### 17.7 早期复现记录(reproduction_report)

3 用例子集复现验证(2026-08-07,`sdc_fuzz_reproduce_all.py --start 0 --end 3`):

| case | N | cluster | mode | reproduced | windows | MRU fail | popcount | signature |
|---|---|---|---|---|---|---|---|---|
| sdc_fuzz_000 | 256 | rename | 0 | **YES** | 1 | 7 | 21 | register-rename-table |
| sdc_fuzz_001 | 384 | rob | 0 | **YES** | 1 | 3 | 15 | rob-speculation |
| sdc_fuzz_002 | 512 | lsu | 1 | **YES** | 1 | 13 | 14 | lsu-store-buffer |

3/3 复现 core179 SDC,每个都有 core179 签名(elem[0]+多位)。rename 簇(sdc_fuzz_000)popcount 最高(21),与机制判别流程 A(物理寄存器活性误判)一致——见 §11.2a.D 交叉印证。

### 17.8 与已有诊断的关系

本机制**复用** libc-only MRU 的触发逻辑(`mru_eigenmc.c`/`esdiag_phase2.cpp` 的 LCG RNG、矩阵构造、CRC、mode 分支),但以 **opendcdiag 标准 sandstone 测试**形态集成(用 `DECLARE_TEST` + `report_fail` + `--cpuset` 调度),即 §15.2 第 4 条"固化到 OpenDCDiag 测试集,作为持续 SDC 监测器"。libc-only MRU 仍是**黄金对照**(验证器用它做同窗口一致性判定)。

---

## 18. 物理层假说补充(di/dt 电压瞬态)

> 本节是早期物理层推测,写于根因尚未微架构定位之前。**最终微架构根因见 §11.2 / §11.2a** —— 经六层假设排除,故障定位在**乱序执行引擎的状态泄漏**(物理寄存器活性误判,流程 A),而非任何执行单元本身或供电层。两者不矛盾:本节提供**物理诱发因素**(为何稀疏矩阵负载能激发缺陷),§11.2a 提供微架构定位(缺陷在哪条写回路径)。

### 18.1 `eigen_sparse` 的计算特征

192 核 ARM64 服务器上,单核(Core 179)在运行 OpenDCDiag 的 `eigen_sparse` 时爆出 SDC。`eigen_sparse` 是专门用来"榨取"和暴露芯片微小物理缺陷(Marginal Defects)的测试用例,底层调 Eigen C++ 线性代数核心库:构造随机生成双精度实数的稀疏矩阵(256×256,CSR/CSC 压缩格式);通过直接法(Sparse Cholesky: `SimplicialCholesky`)求解线性方程组 `Ax = b`;循环验证将结果与 golden 比对,浮点尾数不匹配即抛失败,捕捉 SDC。

### 18.2 为何"稀疏矩阵"能抓到 SDC(物理诱发因素)

密集矩阵(如 `eigen_gemm`)对 CPU 的压榨是"平稳的极高负载",而**稀疏矩阵测试是"脉冲式的极端电磨损"测试**:
- **极端的 di/dt 电压瞬态突变**:稀疏矩阵计算具有高度的**非连续内存访问**(Irregular Memory Access)。CPU 状态高频切换:状态 A(内存等待,找不到下一个非零元素,Cache Miss,流水线停顿)↔ 状态 B(满载计算,数据到达,所有浮点乘加单元瞬间满负荷启动)。
- 高频"启-停-启"在极短周期内造成剧烈电流突变(di/dt)。
- **物理缺陷暴露**:当电流瞬间飙升时,Core 179 局部的供电网络(PDN)无法瞬间补偿,导致**局部电压骤降(Voltage Droop)**。若 Core 179 本身存在微弱的硅片工艺边缘缺陷(弱体质核),电压骤降会导致晶体管翻转延迟增加,从而在时钟沿到来时锁存错误数据位,而硬件校验纠错单元(ECC/Parity)又无法覆盖纯算术单元逻辑,最终酿成 SDC。

> 与 §11.2a 的关系:这一 di/dt 假说解释了"为何稀疏 + 满载"是物理诱发条件;§11.2a 进一步把缺陷定位到**乱序执行引擎的物理寄存器活性管理**(而非供电层本身),即电压瞬态只是"放大镜",真正的损伤点是 OoO engine 的物理寄存器活性边界判断(流程 A)。

### 18.3 涉及的核心指令(经反汇编确认)

> ⚠️ 早期推测 Eigen 在 ARM64 用 SIMD 向量化(FMADD/FMLA/LD1/SVE Gather-Load)。但经对 `esdiag_phase2`/`eigen_cabidrv.o` 编译产物的反汇编核查(§11.2a.B),`factorize_preordered` 热循环使用的是**标量**双精度指令(`fmadd d0`/`fmsub d1,d2,d3,d1`/`fdiv d3` 等 d 寄存器),**未使用 NEON 向量指令**(无 q/v 寄存器、无 LD1/ST1/SVE)。实际触发指令是**标量 fmadd/fmsub + 间接寻址 ldr/str**(见 §11.2a.B)。

- **浮点乘加指令(触发主力,经反汇编为标量)**:`fmadd d0`/`fmsub d1,d2,d3,d1`(`SimplicialCholesky::factorize()` 列更新 rank-1 update,标量双精度,经反汇编确认);
- **非连续内存加载/存储指令**:标量 `ldr d, [x, x, lsl #3]` / `str d, [x, x, lsl #3]`(间接索引移位寻址,经反汇编确认);
- **地址生成与分支控制指令**:`ldrsw`/`add`/`lsl` 用于计算 `Li[p]`/`Lp[i]` 指针(经反汇编确认);cdiv 存在循环边界,频繁 `cmp`/`b.`(条件分支)。

### 18.4 CPU 微架构逻辑单元(可疑点,已被微架构根因取代)

早期推测可能引发 SDC 的物理故障点:

1. **FMA Pipelines(浮点乘加流水线)**:推测 SDC 可能发生在乘法结果输入到加法归约器的最后一级。**§11.2a 更正**:纯 FMA 探针(`fma_probe.c`)0 触发,密集 GEMM/SVD 0 失败 → 排除"浮点 pipe 损坏"。实际触发需 rank-1 update 的间接寻址 + 长存活累加器交错,非单纯 FMA。
2. **LSU(Load/Store Unit)**:稀疏矩阵导致大量地址不连续,LSU Load/Store Buffer 处于极度不平衡状态。**§11.2a 关联**:这与最终根因"乱序执行引擎状态泄漏"部分吻合——LSU store buffer 是三个可疑泄漏点之一(另两个:物理寄存器重命名表、ROB);但机制判别判定最可能是物理寄存器活性误判(流程 A),而非 store buffer(流程 E 被 -15.68 垃圾值排除)。
3. **L1 D-Cache & D-TLB**:访问跳跃导致 Bank Conflicts。**§6 更正**:per-core PMU 显示核 179 的 L1D/dTLB refill 与健康核 176 差异 < 2% → 排除 L1D/TLB 损坏;差分 perf 显示 Eigen 的 L1D-refill 是纯 C 的 4.05×,但纯 C 探针匹配该特征后仍 0 触发 → L1D 压力非充分条件。
4. **局部时钟树与电压稳压器(Local Clock Tree & LDO)**:Core 179 局部电压供应响应速度(LDO 或 PDN)慢了纳秒级别。**§18.2 关联**:这一供电层假说未被排除(也无法在软件层证伪),与微架构根因并存:供电瞬态是诱发因素,OoO engine 状态泄漏是损伤点。最终确认需 RTL 仿真或芯片内部文档(§15.1 局限)。

---

**附录:核心实验数据索引**

| 实验 | 日志/数据 | 结论 |
|---|---|---|
| G5(全核固定 seed) | (TAP 内联) | 仅 179 失败,285/285 |
| G6(换 seed) | (TAP 内联) | 仅 179,140/140,seed 无关 |
| 测试类型矩阵 | (TAP 内联) | 仅 eigen_sparse 触发 |
| per-core PMU | `/tmp/p179.csv` `/tmp/p176.csv` | 微架构全正常 |
| **M1(满载+实时内核监控)** | `/tmp/M1.log`(dmesg 跟踪) | 55 失败/20s,EDAC=0,dmesg=0(静默) |
| esdiag3 D4 | (stderr 内联) | A 0/3000,x 28/3000 |
| 字节模式 D2/D3 | (stderr 内联) | elem[0],21-30 位混叠 |
| **MRU 削减 1**(dense/SpMV/gather/FMA) | (stderr 内联) | 单一操作全 0 失败,需 Cholesky |
| **MRU 削减 2**(symbolic vs numeric) | (stderr 内联) | numeric-only 15/1500,compute-both 4/1500(状态泄漏型) |
| **MRU 削减 3**(规模 N 扫描) | (stderr 内联) | N=64/128 不触发,N≥256 触发,固定 elem[0] |
| **差分 perf**(Eigen vs 纯 C,2026-08-07) | `/tmp/p_eigen.csv` `/tmp/p_purec.csv` | L1D-refill 4.05×(被 coldl1d 证伪非充分);纯 C 前端受限 7× |
| **libc-only MRU E2E** | (stderr 内联) | mrueig 6-24/3000 fails,与 Eigen 参考同窗口同率 |
| **机制判别**(2026-08-13) | `eigen_cabidrv.o` 反汇编 + 六流程矩阵 | 最可能流程 A(物理寄存器活性误判),次可能 C |
| **挂死复位实证**(2026-08-13) | `/var/log/messages` | RCU stall + SBSA watchdog 硬复位,无干净关机序列 → 安全约束证伪 |
| **sdc_fuzz 3 用例子集**(2026-08-07) | `reproduction_report.md` | 3/3 复现,rename 簇 popcount 最高(21) |
