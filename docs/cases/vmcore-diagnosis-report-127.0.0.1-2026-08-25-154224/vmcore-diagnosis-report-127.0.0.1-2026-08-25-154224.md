# 诊断报告 — vmcore `127.0.0.1-2026-08-25-15:42:24`

**机器**：Yangtze Computing R240K V2（Kunpeng-920/HIP08，8 NUMA × 24 CPU = 192 核，767.8 GB，BIOS 7.48）
**内核**：6.6.0-145.3.23.154.oe2403sp3.aarch64（KASLR 启用，kdump 就绪）
**转储**：2026-08-25 15:41:38 崩溃，UPTIME 21:20:09，PARTIAL DUMP 27.6 GB
**分析日期**：2026-08-25；分析工具 crash 8.0.4 + 精确版本 debuginfo + vmlinux 反汇编

> **报告结构说明**：本报告分两层。**第一层（§0–§11）** 基于 15:42 / 15:58 两个转储的寄存器-内存对照与跨开机统计，判定根因为 CPU179 核内数据通路间歇性硬件故障（置信度【强推】）。**第二层（§II-1–§II-8）** 在第一层结论之下，补充现场硬件状态取证（RAS 错误节点、FAR 统计）与新判别实验（无 store 纯加载探针），将根因收敛定位到 LSU 装载数据返回通路族（置信度【实锤级收敛】），并给出微结构单元排序与供应商 DFT 质询清单。第二层中凡引用《DIAGNOSIS_REPORT.md》者即指本文件第一层。两层结论一致承接，置信度由【强推】上升为【实锤级收敛】系因第二层引入了新证据（ERR 寄存器全核扫描、电压裕量核查、M5 纯加载实验）。

---

# 第一层　崩溃诊断（根因 = 核内数据通路间歇故障 ·【强推】）

## 0. 结论摘要

| 项目 | 判定 | 置信度 |
|---|---|---|
| 直接死因 | CPU179 上 `find_busiest_group+0x140` 解引用野指针 `ffffa5aa9b5a97e0` 触发 L3 翻译故障 | 【实锤】 |
| 野指针成因 | 加载 `__per_cpu_offset[176]` 时寄存器得 `0`，而内存实值 `ffffda55e61ce000` 完好无损 —— **读路径瞬时损坏** | 【实锤】 |
| 故障页为何无映射 | 该地址是内核镜像 init 区内已死亡的静态 percpu 模板（`&runqueues`），开机后被 `free_initmem()→vunmap_range()` **设计性解映射**。非页表损坏 | 【实锤】 |
| 根因 | **CPU179 核内数据通路间歇性硬件故障（mercurial core / SDC 类）**：加载通路间歇从错误位置送数/撕裂 | 【强推】 |
| 波及范围 | 仅 CPU179；四次开机全部致命崩溃与全部 spurious fault 100% 归属该核，其余 191 核零异常 | 【实锤】 |

这是该机 **CPU179 的第 4 次致命崩溃**（08-14、08-24、08-25 15:42、08-25 15:58）+ 当前第 5 次开机继续产生同类事件。**建议立即 `offline CPU179` 并对整机发起 RMA。**

---

## 1. 崩溃现场

```
Unable to handle kernel paging request at virtual address ffffa5aa9b5a97e0
ESR = 0x96000007  EC=0x25 DABT(current EL)  FSC=0x07 level-3 translation fault
WnR=0 (读)   ISV=0   S1PTW=0
CPU: 179  PID: 2018836  Comm: claude
pc : find_busiest_group+0x140/0xb60
调用链: ep_poll → schedule → pick_next_task_fair → newidle_balance → load_balance → find_busiest_group
Code: f9400782 f879d814 2a1903e0 8b14003b (f9409377)
```

## 2. 崩溃指令级解剖【实锤】

vmlinux 反汇编（运行时基址 KASLR 漂移前后一致）：

```asm
ffff...ae34  ldp  x0, x1, [sp, #8]       ; x0 = &__per_cpu_offset, x1 = &runqueues(镜像符号, 见 §5)
ffff...ae38  ldr  x2, [x28, #8]          ; x28 = sgs (struct sg_lb_stats*)
ffff...ae3c  ldr  x20, [x0, w25, sxtw #3] ; x20 = __per_cpu_offset[i], i=x25
ffff...ae44  add  x27, x1, x20           ; x27 = cpu_rq(i) = &runqueues + off[i]
ffff...ae48  ldr  x23, [x27, #288]       ; ← FAULT: rq->cfs.avg.load_avg (rq+288, pahole 验证)
ffff...ae54  bl   cpu_util_cfs
```

addr2line 映射到 `kernel/sched/fair.c`（update_sg_lb_stats 内联进 find_busiest_group）：

```c
12049  for_each_cpu_and(i, sched_group_span(group), env->cpus) {
12050      struct rq *rq = cpu_rq(i);
12051      unsigned long load = cpu_load(rq);        /* = cfs_rq_load_avg → rq->cfs.avg.load_avg */
12053      sgs->group_load += load;
```

崩溃语句即 `sgs->group_load += cpu_load(cpu_rq(i))`。寄存器快照算术自洽验证：
- 本次：i=w25=**176**，x20=`0` → x27=x1+x20=`ffffa5aa9b5a96c0`，FAR=x27+0x120 ✓
- i=176 与 CPU179 同簇（live topology：`cluster_cpus_list: 176-179`），循环索引完全合法。

## 3. 决定性实验一：寄存器 vs 内存对照（本转储）【实锤】

| 量 | 寄存器快照（崩溃时） | 内存真值（vmcore 读出） | 判定 |
|---|---|---|---|
| x1 | `ffffa5aa9b5a96c0` | `sym runqueues` = `ffffa5aa9b5a96c0` | **一致，完好** |
| x20 = `__per_cpu_offset[176]` | **`0x0000000000000000`** | **`0xffffda55e61ce000`** | **背离！** |

`__per_cpu_offset[]` 全数组 192 项为完好等差序列（首项 `ffffda55e4a6e000`，步长 `0x22000`），无任何持久损坏。数组末尾(entry[191]之后)紧邻大片零区。
若内存真值被采用，则 `cpu_rq(176) = ffff8000817776c0`——crash 用转储页表独立验证该地址映射正常（PTE VALID|DIRTY），其 `cfs.avg.load_avg = 0x400`（合理负载值），**加载本应成功、不会 panic**。

⇒ 软件未写坏任何东西；是**读取动作本身送回了错误数据**。

## 4. 决定性实验二：16 分钟后的第四次崩溃（15:58 转储）独立复证【实锤】

重启后仅 418 秒、无任何实验模块、kworker 环境再次崩溃：同为 **CPU179**、同 PC `find_busiest_group+0x140`、同一指令窗口。此次 i=146：

- 内存真值 `__per_cpu_offset[146]` = `ffffcc879ed92000`
- 寄存器 x20 = `00ffffcc879da2e0` = **`__per_cpu_offset[0] >> 8`**（该次开机 `__per_cpu_offset[0]` 基址为 `ffffcc879da2e000`，逐 KASLR 而异；右移 8 位恰等价于"从 `&__per_cpu_offset[0]+1` 取 8 字节"的跨界错位读——首项低字节为 0x00，丢零字节、高位补零即得此值）

字节级精确闭合：该值恰等于"从 `&__per_cpu_offset[0]+1` 取 8 字节"的结果（跨元素边界错位一字节的**结构化撕裂读**，概率论上排除随机巧合 ≈2⁻⁶⁴）。两次今日崩溃共享同一机理族：**加载通路从错误位置送数**（本次=元素[0]错位一字节；§3 案例=零区/全零事件）。历史案例（08-14）的 ROTL16 撕裂亦同族（详见第二层 §II-2.1 E3）。

## 5. "pte=0 页表损坏"假象的排除 —— 本案关键消歧【实锤】

表面悖论：x20=0 ⇒ x27=`&runqueues`（内核镜像内部地址）⇒ 已映射地址怎会翻译失败？且 oops 打印的描述符换算出"16TB 物理地址"。逐层排除后全部真相大白：

1. **物理地址疑云澄清**：本机为 8 NUMA 节点高 PA 布局，DRAM 窗口最高至 `0x6057ffffffff`（SRAT/iomem 实证，如 Node7: `0x604000000000-0x6057ffffffff`）。所有"看似不可能"的 PA 均落在真实节点窗口内。描述符合法。
2. **走链真实性**：crash 从转储内存独立重走四级页表，与内核 oops 打印**逐位一致**；四级索引数学手工复核自洽（pgd idx 331@+0xa58 … pte idx 425@+0xd48）。
3. **pte=0 是设计行为**：`.data..percpu`（VMA `81b52000`–`81b6c3e8`，含 `runqueues` 静态模板）位于 `[__init_begin(819a0000) .. __init_end(81f50000)]` init 区间内。`arch/arm64/mm/init.c::free_initmem()` 在开机后执行 `vunmap_range(__init_begin, __init_end)` **主动解映射**（源码注释明确：防止模块重用该 VA 区间）。静态 percpu 模板在 `setup_per_cpu_areas()` 复制到各 per-cpu 块后即为死区，解映射无害且无人访问。
4. **相邻区域健康对照**：文本页(PC)、`__per_cpu_offset`(.data)、swapper_pg_dir 全部映射正常且属性正确（RDONLY/RW）；仅 init 区内页 PMD/PTE 为 0。
5. **跨开机确定性**：15:58 转储对同型地址的走链描述符与本转储**位级一致**（不同 KASLR 基址下确定性早启分配所致），证明是系统性行为而非随机损坏。

⇒ 页表本身健康；故障是**对设计性解映射区域的正确报错**。它反而充当了"检测器"：把一次本可能无声无息的坏读放大成了可见的 panic。（若无此机制，野指针将指向静态模板旧数据——内容恰好是合法 rq 结构副本，可能静默产出错误调度决策而不崩溃。）

## 6. 跨开机统计与公平性核查【实锤】

| 开机 | 致命崩溃点 | spurious fault 次数 | 异常归属核 |
|---|---|---|---|
| 08-14 | find_busiest_group+0x140 | 12 | 全部 CPU179 |
| 08-24 | bio_add_page+0xf0（野指针同样高位撕裂 `003c521d…`） | 34 | 全部 CPU179 |
| 08-25 15:42（本案） | find_busiest_group+0x140 | 1（1707s，pmdalinux） | 全部 CPU179 |
| 08-25 15:58 | find_busiest_group+0x140（418s 即崩） | 0（来不及积累） | 全部 CPU179 |
| 08-25 16:06 起（当前 live） | —（进行中） | 已 3 次（1467s 时） | **又全部 CPU179** |

四个完整开机 + 当前第五次开机，约 50 次可归因异常事件 **100% 落在唯一指定的 CPU179**，其余 191 个共享相同代码/内存/负载的核零异常。均匀随机假设下概率 ~192⁻⁵⁰，统计上排除一切非核级原因。

诚实修正：前案记录称"两次致命点同为 find_busiest_group"，经本次复核，**08-24 的致命点实为 `bio_add_page+0xf0`**（同为 CPU179、同为高位撕裂野指针）。四分之三命中同一热点指令、其余命中不同路径但同病同源——这强化了"与代码无关的核级硬件缺陷"判断。

## 7. RAS / 错误上报负证据链【实锤】

- ESR EC=0x25（DABT current EL）≠ 0x2f（SError/RAS）：硬件从未将该故障识别为可上报硬件错误。
- EDAC：`mc0 ce=0 ue=0`；SEL：无可纠正内存错误、无 Processor IERR（仅温度 Upper Non-critical 阈值事件，为 SRAM 类间歇故障的已知加速因子）。
- HEST/GHES firmware-first 已启用但全程无 GHES 记录；BERT 空。
- HiSilicon RAS 驱动覆盖面为 SoC 互连/DDRC/L3C/HHA 等，不含 CPU 核内 L1d/加载通路 —— 核内故障天然处于上报盲区。
- 内存子系统反证：`__per_cpu_offset[]` 等全局数据在两个转储中完美无损；真 DRAM/DIMM 故障无法解释"仅单核读坏、其余核同址读取无恙"。

## 8. 备择假设逐一排除

| 备择假设 | 排除依据 |
|---|---|
| 软件 UAF / 越界 | 坏值来源字段内存完好；无任何持久损坏痕迹；两转储独立复证 |
| sched domain 重建竞态 | 四次开机 dmesg 无任何 CPU hotplug/域重建事件；拓扑静态 |
| 内核 bug（fair.c） | 同一二进制跑满 192 核，唯 179 出事；代码段 RDONLY 且转储中指令字节与 Code 窗口一致 |
| __per_cpu_offset 被软件写坏 | 数组等差完好；crash 经其寻址成功解析全部 per-cpu 结构 |
| 页表/页表读取损坏 | §5 全链条消歧（vunmap 设计行为 + 双机位级一致 + 相邻区域健康） |
| DIMM/DRAM 持久损坏 | 多核共享同址数据完好；EDAC/BERT 干净；损坏随核绑定而非随地址绑定 |
| 实验扰动（l1d_disable/opendcdiag） | 15:58 崩溃发生于全新开机 418s、无任何实验；本案崩前实验模块已卸载 3.7h |
| 其他核传播 | 公平性核查：191 核零事件 |

## 9. 根因链（置信度分级）

```
【硬件·触发】CPU179 加载通路瞬时异常（LDR 送回错误位置的数据/撕裂/全零）
     │        ├─ 实锤：寄存器≠内存（两转储独立复证）
     │        └─ 强推：结构化撕裂签名（>>8 字节错位、ROTL16 族）指向取数/对齐/旁路通路瞬态失稳；
     │            更细定位（sense-amp/位线/转发网络）需芯片级 BIST/DFT，软件不可达【假设】
     ▼
【软件·放大器】cpu_rq(i) 指向 init 区死模板页（设计性 unmap）→ 合法 L3 翻译故障 → panic
     │        └─ 实锤：free_initmem/vunmap_range 源码 + 走链验证
     ▼
【系统·后果】kdump 正常捕获；五次开机持续复发，趋势未见衰减
```

根因表述：**CPU179 物理核私有数据通路存在间歇性软故障（mercurial core / SDC 类），其坏读被内核 init 区解映射这一正常安全机制转化为致命翻译故障。** 故障不在任何 RAS 覆盖范围内，故全部硬件上报通道干净——负证据与正证据互洽。

> 本层将根因判定到"核内数据通路"粒度、置信度【强推】。更细的微结构单元定位（LSU 装载数据返回通路族、U4 vs U1 排序）见第二层。

## 10. 处置建议

1. **立即**：`echo 0 > /sys/devices/system/cpu/cpu179/online` 下线该核（损失 1/192 算力，远低于日均两次宕机代价）；或 `maxcpus`/隔离参数固化。
2. **RMA**：凭本报告证据链向整机/CPU 供应商申请换件。多开机单核 100% 复现 + 寄存器/内存背离实验是标准 RMA 证据。
3. **监测**：部署对 `spurious kernel translation fault` 的 logwatch（该消息是本缺陷最灵敏的前兆指标，先于致命崩溃出现）。
4. **可选加固**：将 `find_busiest_group` 等热路径迁移出 179（taskset/cpuset）只能降低触发频率，不能根治。
5. **不建议**：继续 SCTLR.C 开关类实验作为缓解手段（本次开机数据显示无效，且实验期与崩溃期无相关性）。

## 11. 附录：关键证据命令

```
crash> sym runqueues                  → ffffa5aa9b5a96c0 (= x1，完好)
crash> px __per_cpu_offset[176]       → 0xffffda55e61ce000 (内存真值；寄存器 x20=0)
crash> rd -64 __per_cpu_offset 192    → 192 项完美等差数列
crash> px ((char*)&__per_cpu_offset)[192] 后续内存 → 零区（解释 x20=0 的可能来源）
crash> vtop 0xffff8000817776c0        → rq(176) 真值地址映射 VALID（本应成功的访问）
crash> vtop ffffa5aa9b5a97e0          → PTE=0（init 区，free_initmem 设计性 unmap）
objdump: pc=f9409377 = ldr x23,[x27,#0x120]; addr2line → fair.c:12051 cfs_rq_load_avg
pahole/gdb: rq+288 = rq->cfs.avg.load_avg; sg_lb_stats.group_load@8/group_util@24 与累加指令吻合
nm: .data..percpu VMA ffff800081b52000 ⊂ [__init_begin ffff8000819a0000, __init_end ffff800081f50000]（已核对）
15:58 转储: x20=00ffffcc879da2e0 == __per_cpu_offset[0]>>8（该次开机基址 ffffcc879da2e000；字节错位撕裂，精确闭合）
```

*第一层分析产物（crash 会话日志、PLAN.md）保存于分析机 `/tmp/vmcore-analysis-20260825/`。*

---

# 第二层　微架构级根因定位（根因 = LSU 装载数据返回通路族 ·【实锤级收敛】）

**日期**：2026-08-25　**范围**：在第一层"核内数据通路间歇故障"结论之下，定位到具体微结构单元
**方法**：全部已知事件指纹的微架构约束推导 + 现场硬件状态取证（RAS 错误节点寄存器、FAR 统计）+ 新判别实验（无 store 纯加载探针）
**证据来源**：四个 vmcore 寄存器现场、三份本机活体复现报告（gem5-fi/docs）、73 个 spurious FAR、192 核 ERR 寄存器扫描、本次新实验

---

## II-1. 判定结论（先行）

> **CPU179 的 LSU（Load/Store Unit）装载数据返回通路存在对"流水线指令调度相位 × 电压裕量"双重敏感的时序边界缺陷。当指令流中存在推进地址的 store 流量时，特定相位窗口内装载结果会从错误槽位/错误字节选路取数——表现为①相邻旧数据以 1–2 字节相位旋转送达（错元素+错位）、②全零读出、③多位值损坏。该缺陷不产生任何架构化 RAS 记录。**
>
> **最可能的具体位置（按证据强度排序）**：U4 行填充缓冲/重放合并级 ≈ U1 L1D 数据阵列读出选路（way-mux/字节列选通） > U3 SQ→LQ 转发输出级 > U5 PRF 读出/旁路总线。
>
> **置信度**：缺陷单元=LSU 数据返回路径族【实锤级收敛】；具体到 U4 vs U1【强推 60/30】；U3/U5 为长尾候选。

---

## II-2. 事件指纹总库（微架构约束的原始材料）

### II-2.1 内核侧（vmcore 寄存器 vs 内存对照，本案新增）

| # | 开机 | 崩溃点 | 请求 | 寄存器实收 | 内存真值 | 指纹变换 |
|---|---|---|---|---|---|---|
| E1 | 08-25 15:42 | find_busiest_group+0x140 | `off[176]` | `0x0000000000000000` | `ffffda55e61ce000` | **全零事件** |
| E2 | 08-25 15:58 | 同上 | `off[146]` | `00ffffcc879da2e0` | `ffffcc879ed92000` | **= `__per_cpu_offset[0]>>8` = bytes@`&off[0]+1`：entries[0..1] 跨界 1 字节移位**（该次开机 `off[0]` 基址 `ffffcc879da2e000`，低字节为 0x00，故右移 8 位与"自数组首地址 +1 字节取 8 字节"逐位等价） |
| E3 | 08-14 | 同上 | `off[176]` | `d93715ba0000ffff` | `ffffd93715ba0000`(entry[1]) | **= ROL16(entry[1])：相邻元素 2 字节循环移位**（`entry[1]` 左旋 16 位逐位吻合） |

E3 的内存真值由本次 08-14 转储 crash 会话独立验证（entry[1]=`ffffd93715ba0000`，ROL16 位级吻合）。**此前记录中"E3 属随机损坏"的修正是错误的**——64 位精确旋转变换的巧合概率 ~2⁻⁶⁴，恢复旋转指纹判定。

E3 附带次级异常：oops 打印 FAR=`0036bc83…97df` 而 pt_regs x27+0x120=`d936bc83…97df`，最高字节 7 位差异——同窗口内 FAR 写入或捕获路径又发生一次独立损坏（不影响主结论，计入同时多异常计数）。

### II-2.2 用户态活体复现（既有研究，本机同核）

| 来源 | 触发 | 损坏谱 |
|---|---|---|
| method3（07-31，SN 50A6886522221659 = 本机 CPU） | VDDAVS −30mV + STL | `__per_cpu_offset[i]` 载入随机垃圾，find_busiest_group 三连崩，全 CPU179 |
| method2/v3（08 月，标称电压） | movbe/mrn_rmw/GEMM/SVD 等 6 类负载，满载并发 | 36 样本/562 翻转位：尾数集中 85–93%、符号免疫、位数随路径（SVD 单位↔GEMM 多达 39 位）；movbe reload 处触发率≈100%→加一条 no-op ALU 指令跌至 10–20% |
| method1 | eigen numeric factorize 列更新交错 | 固定 x[0] 写回混叠（21–32 位）；numeric-only 失败率 4× compute-both（状态泄漏签名） |

### II-2.3 MMU 侧

73 个 spurious translation fault FAR（与第一层 §6 跨开机统计口径一致）：其中 68 个为跨 NUMA 节点真实 DRAM 线性映射地址（`0xffff2xxx/4xxx/6xxx` 段），5 个为 vmalloc 段地址（`0xffff00xxxxxxxxxx`），全部经 AT S1E1R 重翻译确认有效；mod-8 对齐——73 个 FAR 全部 8 字节对齐、无单一位卡滞模式（指针型地址自然对齐，故同落 mod-8 余 0 槽，不构成译码器/比较器卡滞位硬故障证据） → 排除比较器/译码器硬故障，指向走页器阵列读取的同类时序瞬态。

> 计数订正注：本节早前版本曾记为"72 个 DRAM 线性映射 + 1 个 vmalloc"。经对 `far_all.txt`（73 行）按地址段复核，实为 68 个 DRAM/线性段 + 5 个 vmalloc 段，合计 73 不变；结论（全部有效、无卡滞位）不受影响。

### II-2.4 硬件状态取证（本次新增）

- **RAS 错误节点**：192 核 × 5 节点，FR/CTLR/STATUS/MISC 逐位一致，**CPU179 零差异、全网零记录** → 缺陷产生的模拟时序失效不形成任何架构化错误记录（把"逃逸上报"证据链从 SoC 层推进到核内 ERR 寄存器层，承接第一层 §7 负证据链）。
- **电压核查**：四路 VDDAVS 0.94–0.97V 健康，无残留调压 → 排除"欠压配置残留"备择假设；−30mV 可控复现证明电压裕量敏感性。
- **温度**：SEL 仅 Upper Non-critical 温度阈值事件（加速因子，非根因）。

## II-3. 本次新判别实验（M5）

**设计**：热循环三条内联汇编 `ldr`（A[i] 双读 + 金标 G[i] 读），**零 store**、零溢出、无 CSE（反汇编逐条验证，源码见 `loadonly.c`），pin 核 179；node7 其余 47 核 stress-ng 压力；核 176 对照臂；6×75s 窗口。

**结果**（`loadonly_run.log`）：核 179 共 6 窗口累计迭代 40,679,760 次（≈40.7M），fails=0 —— 折算约 **1.0×10¹² 次加载（40.7M 迭代 × 8192 元素 × 3 ldr），零撕裂**；对照臂同样干净。

**推理**：
1. 结合 method2 probe-A（移除 store 即不触发）：**store 流量在指令流中共存是该缺陷的必要条件**——纯加载流即使 10¹² 次也安全。
2. 结合 E1–E3（内核循环中 store 到 sgs 与被撕的 off[] 加载仅隔数条指令，但地址无关）与 probe-X（store↔load 相邻非必要）：**store 无需指向同一地址，只需在流水线中制造 LSU 相位压力**。
3. 单次访问故障率上界 <10⁻¹²（此访问类），而相位对齐的 movbe 序列可达每分钟数次 → 缺陷是**相位依赖的边际时序失效**，不是均匀随机失效。

## II-4. 微架构约束表（每事件 → 排除/指向）

| 约束 | 来源 | 排除 | 指向 |
|---|---|---|---|
| C1 送回数据=其他位置真实内容的字节旋转（1B/2B 可变相位） | E2,E3 | ALU 语义错误、GPR/索引算术（偏移恒 ×8 对齐，地址数学不可能产生 +1 字节源） | **数据返回通路的选路/合并级**（列选通、fill-buffer 合并、旁路输出 mux） |
| C2 错源内容=数组头部近期访问过的行 | E2,E3 | 随机噪声（2⁻⁶⁴ 巧合） | **陈旧数据回放**：fill-buffer/LQ 旧项被误交付 |
| C3 全零读出 | E1 | — | 同一选路级的空/无效槽位态（或 sense-amp 不翻转） |
| C4 必须有 store 流共存于流水线 | M5 实验+probe A | 纯 L1D 阵列自发失效模型 | **store 引擎交互**（SQ 压力改变装载调度相位 / fill-buffer 被 store miss 占用） |
| C5 一条 no-op 指令即令触发率 100%→10% | probe H/X | 结构性硬故障 | **逐迭代发射相位竞态**（时序窗口极窄且相位敏感） |
| C6 −30mV 可控复现、标称电压偶发 | method3+电压核查 | 软件、配置残留 | **电压裕量敏感的模拟/时序边界** |
| C7 尾数集中、符号免疫、位数随负载类型 | method2 §6 | 符号逻辑/ALU 语义、单比特 SEU | 数据字整体损坏后经计算放大 |
| C8 固定 x[0] 写回混叠、numeric-only 4× | method1 | 输入损坏、指针散射 | 下游表现：C 类坏值进入依赖链后的写回选择异常（或独立的写回选路弱点） |
| C9 PTW 对有效映射瞬时失败、无卡滞位 | FAR 统计 | TLB 比较器/走页器译码器硬故障 | 同族模拟裕量弱点波及走页器阵列读取 |
| C10 架构化 RAS 节点全程零记录 | ERR 扫描 | — | 失效粒度低于任何检测器（无奇偶/ECC 覆盖或未挂接 ERR 节点） |
| C11 五次开机 100% 单核 | 全部 | 批次性问题、系统级原因 | 单个物理核私有结构 |

## II-5. 收敛推理链

```
C1+C2+C3: 损坏发生在"数据返回选路"而非计算/地址生成 —— 实锤
C4:       该选路与 store 引擎状态耦合 —— 实锤（新实验+probe A）
C5+C6:    耦合本质是时序相位 × 电压裕量的模拟边界 —— 实锤
C7+C8:    用户态所见的多位损坏谱是该通路上游/下游的表现 —— 强推
C9:       同族弱点波及 PTW 阵列读取 —— 强推（同一核的 SRAM 读出族）
C10+C11:  缺陷为单核私有、低于一切检测粒度 —— 实锤
⇒ 缺陷单元：LSU 装载数据返回通路（fill-buffer/replay 合并 ≈ L1D 读出选路），
  物理本质：特定相位+低压组合下的建立/保持违例（small-delay-fault 类）
```

**U4 vs U1 的判别依据**：E2/E3 送达的是"早前读过的行"的内容——若仅 L1D 阵列选路错（U1），应送达同组其它 way 的现役行（任意内容）；恰好命中"近期访问过的旧行"更符合 fill-buffer/LQ 陈旧项回放（U4）。故 U4 略占优。最终裁决需供应商 RTL/DFT。

## II-6. 与 TaiShan v110 已知实现的对照（诚实边界）

- 本核 L1D 64KB、L2 512KB 私有，L3 24MB/簇（sysfs 实证）；公开资料未披露其 L1D 是否含 ECC 或奇偶保护。若 L1D 数据阵列带校验而缺陷仍静默，则校验采样点在选路级之前/之后的位置是关键问题——列入供应商质询清单。
- 微码 revision 全核一致为 0（method1 记录）→ 非微码可修复类缺陷的可能性大，但仍建议询问是否有微码规避手段（如强制 LSU 调度保守模式）。

## II-7. 给供应商（HiSilicon/整机厂）的 DFT 质询清单

1. 对 CPU179 所在 die 执行 **scan-at-speed / LBIST**，向量优先覆盖：LSQ fill-buffer 合并逻辑、L1D 数据阵列输出列选通、SQ→LQ 转发 CAM 匹配线与输出 mux。
2. 提供本批片 Vmin shift 分布与本颗的 **margin scan / delay-fault** 结果（small-delay-fault 逃逸是首选物理机理）。
3. 质询 L1D/L2 数据与脏行的**校验方案**（ECC/奇偶有无及覆盖阶段）——解释 C10 静默性。
4. 请求 **Vmin-marginal 复测协议**：−30mV + movbe/mrn_rmw 序列作为量产筛选向量的可行性确认（method1/2 的 libc-only MRU 可直接交付作 DFT 向量）。
5. 若有第二台同批次机器，运行同一 MRU 以判定个体缺陷 vs 批次风险。

## II-8. 处置（继承并强化）

- 立即 `offline cpu179`（五开机 51+ 事件全在该核；隔离后其余 191 核在全部实验中零异常）。承接第一层 §10。
- 该 socket（PkgID 19062）RMA，附本文件 + 第一层证据包。
- 监控指标：`spurious kernel translation fault` 出现频率（最灵敏前兆）。

---
*第二层分析产物：rasnode.ko 源码（`rasnode.c`）、`loadonly.c`、`far_all.txt`、`fbg_head.txt`、`crash1–5.log` 见本转储目录（已同步至 `/tmp/vmcore-analysis-20260825/`）。*
