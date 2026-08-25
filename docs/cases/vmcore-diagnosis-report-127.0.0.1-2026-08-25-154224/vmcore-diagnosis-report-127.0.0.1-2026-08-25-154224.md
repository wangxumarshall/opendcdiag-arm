# 诊断报告 — vmcore `127.0.0.1-2026-08-25-15:42:24`

**机器**：Yangtze Computing R240K V2（Kunpeng-920/HIP08，8 NUMA × 24 CPU = 192 核，767.8 GB，BIOS 7.48）
**内核**：6.6.0-145.3.23.154.oe2403sp3.aarch64（KASLR 启用，kdump 就绪）
**转储**：2026-08-25 15:41:38 崩溃，UPTIME 21:20:09，PARTIAL DUMP 27.6 GB
**分析日期**：2026-08-25；分析工具 crash 8.0.4 + 精确版本 debuginfo + vmlinux 反汇编

---

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
- 寄存器 x20 = `00ffffcc879da2e0` = **`__per_cpu_offset[0] >> 8`**

字节级精确闭合：该值恰等于"从 `&__per_cpu_offset[0]+1` 取 8 字节"的结果（跨元素边界错位一字节的**结构化撕裂读**，概率论上排除随机巧合 ≈2⁻⁶⁴）。两次今日崩溃共享同一机理族：**加载通路从错误位置送数**（本次=元素[0]错位一字节；§3 案例=零区/全零事件）。历史案例（08-14）的 ROTL16 撕裂亦同族。

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
15:58 转储: x20=00ffffcc879da2e0 == __per_cpu_offset[0]>>8（字节错位撕裂，精确闭合）
```

*分析产物（crash 会话日志、PLAN.md）保存于分析机 `/tmp/vmcore-analysis-20260825/`。*
