# 内核崩溃根因诊断报告

| 项目 | 内容 |
|---|---|
| 转储文件 | `127.0.0.1-2026-08-24-18:03:07/vmcore`（116 GB，PARTIAL DUMP） |
| 主机名 | localhost0102 |
| 平台 | Yangtze Computing R240K V2 / BC82AMQA，BIOS 7.48 (2026-06-15)，aarch64 |
| CPU/内存 | 192 CPUs（8 NUMA 节点 × 24），操作系统可见 767.8 GiB（SRAT 总跨度约 6 TB 物理地址） |
| 操作系统 | openEuler 24.03 LTS-SP3，内核 6.6.0-145.3.23.154.oe2403sp3.aarch64 |
| 崩溃时间 | 2026-08-24 18:02:12 CST，开机后 6 天 05:17:42（537462 s） |
| 崩溃 CPU | **179**（MPIDR `0x7A0300`，NUMA Node 7；逻辑编号 176–191 对应簇 0x7A–0x7D） |
| 结论置信度 | 根因为硬件故障（CPU 179 数据通路），置信度：**高（≈95%）** |

---

## 一、结论摘要（Executive Summary）

1. 本次致命 panic 的直接原因是：内核 writeback 路径在 `bio_add_page()` 中将 `bio->bi_io_vec[70].bv_page` 读成了一个非规范野指针 `0x553c521da2e9b99f` 并对其解引用。
2. **决定性证据**：vmcore 中该内存位置实际存储的是完全正常的值 `0xfffffd012d055b80`。即——**内存内容是好的，是执行该加载指令的 CPU 179 返回了坏数据**。这不是软件写坏了内存，而是读取瞬间的数据损坏。
3. 同一机器的历史 vmcore（8 月 14 日、8 月 17 日）显示同类崩溃此前已发生两次：均为 `find_busiest_group()` 中一次按 CPU 索引的数组加载返回撕裂数据。8 月 14 日案例已完成同样的"寄存器 vs 内存"对照：内存中 `__per_cpu_offset[176] = 0xffffd937172de000`（与相邻元素呈完美等距序列），而 CPU 179 实际载入 `0xd93715ba0000ffff`（真值高位片段 + 垃圾低位，典型撕裂读）。
4. 此外，本次开机运行 6.2 天内共出现 **34 次 "Ignoring spurious kernel translation fault" 告警**；连同前两次开机（12 次、26 次），**四次开机累计 72 次，全部发生在 CPU 179 上执行时，无一例外**。此类告警的内核语义是：MMU 报 level-0 翻译错误后，内核用硬件 `AT S1E1R` 指令当场重新翻译同一地址却成功——即页表完好，是页表遍历硬件瞬时报了假错误。这些地址全部位于自启动起永久有效的线性映射区/vmalloc 区。
5. 三个互不相干的子系统（块层 writeback、调度器负载均衡、procfs 读取）、两种独立机制（普通加载返回撕裂数据 + MMU 页表遍历瞬时出错）、四次开机、13 天，所有异常**只**在 CPU 179 执行时发生。

**根因判定：CPU 179（MPIDR 0x7A0300，Node 7）自身的数据通路存在硬件缺陷（核内私有缓存/MMU 加载流水线层级）。建议立即下线该 CPU 并安排硬件维护（RMA）。**

---

## 二、崩溃现场还原（2026-08-24 案例）

### 2.1 故障寄存器与异常帧

```
[537462.654323] Internal error: Oops: 0000000096000004 [#1] SMP
[537462.767631] CPU: 179 PID: 2077673 Comm: kworker/u391:3
[537462.789752] Workqueue: writeback wb_workfn (flush-253:2)      ← dm-2 = openeuler-home (ext4)
[537462.803963] pc : bio_add_page+0xf0/0x1a0
                 x19 : ffff60401b366738   (struct bio *)
                 x22 : fffffd010d971400   (正在添加的 struct page，值正常)
                 x3  : 553c521da2e9b99f   (野指针)
                 x0  : ffff60401dabd460   (= bi_io_vec + 70*16，见下文推演)
Unable to handle kernel paging request at virtual address 003c521da2e9b99f
ESR = 0x9600004, EC = DABT(current EL), FSC = 0x04: level 0 translation fault, WnR=0(读)
```

ESR 解码：EL1 数据访问异常、读操作、level 0（PGD 级）翻译错误；FAR `003c…` 为 x3 去除高字节后的报告值，属"用户与内核地址之间的空洞"，与野指针一致。

### 2.2 故障指令定位（反汇编，vmlinux 带调试符号）

```asm
ffffc360a8592408 <+216>: ldr   x3, [x19, #112]      ; x3 = bio->bi_io_vec
ffffc360a859240c <+220>: sbfiz x2, x0, #4, #32       ; x2 = (bi_vcnt - 1) << 4
ffffc360a8592418 <+232>: ldr   x3, [x3, x2]         ; x3 = bi_io_vec[vcnt-1].bv_page
ffffc360a8592364 (+52)  : ldrh  w0, [x19, #104]      ; w0 = bio->bi_vcnt
ffffc360a8592420 <+240>: ldr   x1, [x3]             ; ← 崩溃指令（Code 尾部 f9400061）
```

调用链：`ext4_iomap_writepages → iomap_writepages → write_cache_pages → iomap_do_writepage → iomap_writepage_map(_blocks) → iomap_add_to_ioend → bio_add_folio → bio_add_page`。

### 2.3 决定性对照实验：寄存器值 vs 内存实际存储

由 `crash> struct bio ffff60401b366738`：

```
bi_vcnt    = 71
bi_io_vec  = 0xffff60401dabd000
bi_end_io  = iomap_writepage_end_bio
bi_bdev    = 0xffff0020271bda00
```

代码读取的是 `bi_io_vec[71-1]` = `0xffff60401dabd000 + 70×16 = 0xffff60401dabd460`，
**与崩溃时寄存器 x0 精确吻合**，证明地址推演无误。随后：

```
crash> struct bio_vec ffff60401dabd460
struct bio_vec {
  bv_page = 0xfffffd012d055b80,     ← 内存中存的是正常 struct page 指针
  bv_len  = 8192,
  bv_offset = 0
}
```

整个 bvec 数组 71 项逐项检视均为合法 vmemmap 指针（`0xfffffd01xxxxxx00 / 0xfffffd81xxxxxx00` 形态），无任何垃圾项。

| | 值 | 说明 |
|---|---|---|
| CPU 179 崩溃瞬间从该地址载入 | `0x553c521da2e9b99f` | 非规范野指针 |
| vmcore 中同地址实际存储 | `0xfffffd012d055b80` | 完全正常 |

两值之间不存在单比特翻转关系，排除偶发位翻转；呈现的是"返回了整条错误数据且局部夹杂真值片段"的形态。

**排除干扰因素**：(a) 故障发生后到 kdump 落盘期间无任何代码会改写该 bvec（次级 CPU 已被停止，故障任务未再执行）；(b) 该 slab 页完整包含于 PARTIAL DUMP 中；(c) 在 `ldr x3,[x3,x2]` 与 `ldr x1,[x3]` 之间仅有写 x2 的 `ubfx` 指令，寄存器 x3 不可能被软件改坏。因此坏值只能来自**加载本身**。

### 2.4 崩溃前 7 秒的伴生 Oops（同一病灶的又一次发作）

```
[537455.068087] Ignoring spurious kernel translation fault at virtual address ffff40295ce624d4
[537455.068586] ... ffff40295ce62185
[537455.072670] ... ffff40295ce62395     （irqbalance 读 /proc/interrupts，seq_printf/__memcpy 触发）
```

三次告警集中在 4.6 秒内且指向**同一个物理页**（PA `0x4029_5ce6_2000` 段，Node 4 DRAM），7 秒后系统即因 CPU 179 的坏读数而崩溃。

---

## 三、"Spurious translation fault" 的机理与本机实证

### 3.1 内核判定语义（openEuler 6.6 源码，arch/arm64/mm/fault.c）

```c
static bool __kprobes is_spurious_el1_translation_fault(unsigned long addr, ...)
{
	...
	asm volatile("at s1e1r, %0" :: "r" (addr));   /* 用硬件 AT 指令当场重翻译 */
	isb();
	par = read_sysreg_par();
	if (!(par & SYS_PAR_EL1_F))                    /* 重翻译成功 ⇒ 刚才的错误是假的 */
		return true;
	...
}
/* __do_kernel_fault(): */
	if (is_spurious_el1_translation_fault(addr, esr, regs)) {
		WARN_RATELIMIT(..., "Ignoring spurious kernel translation fault ...");
		return;
	}
```

即：**每次告警都意味着 MMU 刚刚对一个实际有效的映射报了翻译错误，而紧接着用同一套页表遍历硬件重翻译却成功**。

### 3.2 为什么可以排除软件原因

- 告警地址换算后的物理地址分布在 Node 0/3/4/7 的常规 RAM 及 vmalloc 区（见附录 C）。这些区域的 PGD/PUD/PMD/PTE 自启动建立后**永不改变**——本机 dmesg 无任何内存热插拔动作，线性映射不存在运行时 TLBI 场景。
- 若页表项真被破坏（含 DMA 误写），紧随其后的 AT 重翻译必然同样失败；实测每次都成功 ⇒ 页表始终完好，错误出在遍历硬件的瞬时状态。
- 陈旧 TLB 假说同样不成立：陈旧条目只会提供"旧的有效翻译"造成静默错读，不会凭空产生 level-0 无效翻译；且 AT 指令走同一 TLB 层级，也会命中同一陈旧条目。

### 3.3 统计特征：异常跟随执行 CPU，而非数据位置

四份 vmcore-dmesg 汇总：

| 开机（崩溃日） | spurious 告警数 | 分布 | 自发性 panic |
|---|---|---|---|
| ~08-09 启动（08-11 sysrq 手动触发） | 0 | — | 无（人为 `sysrq-c`） |
| 08-13 启动（08-14 崩溃） | 12 | **全部 CPU 179** | `find_busiest_group+0x140`，kworker/179:1H |
| 08-14 启动（08-17 崩溃，转储不完整） | 26 | **全部 CPU 179** | `find_busiest_group+0x140`，swapper/179（nohz 空闲均衡软中断） |
| 08-18 启动（08-24 崩溃，本报告） | 34 | **全部 CPU 179** | `bio_add_page+0xf0`，kworker/u391:3 |

- 发生告警的进程包括 irqbalance（31 次）、bash（COW 缺页路径 1 次）、writeback kworker 等——irqbalance 在转储时刻运行于 **CPU 181**，证明其并未绑定在 179 上；它 31 次故障全部落在 179 只能解释为：**只有当它在 CPU 179 上执行时才会触发**。
- 告警地址的物理分布（附录 C）：Node 7 ×25、Node 0 ×4、Node 4 ×3、Node 3 ×1、vmalloc ×1，并多次重复命中同一物理页（如 PA `0x604006ed9xxx` 在 16361s/16543s/18323s 三次中招）。若缺陷在 DIMM/内存控制器，无法解释跨节点地址均出错且与其他 CPU 长期读取相同数据无恙；唯一自洽解释是缺陷跟随**执行核**。

---

## 四、历史案例交叉验证（2026-08-14 案例）

### 4.1 现场

```
[113997.348398] Internal error: Oops: 0000000096000004 [#1] SMP
CPU: 179 PID: 1986 Comm: kworker/179:1H   Workqueue: kblockd
pc : find_busiest_group+0x140/0xb60    lr : find_busiest_group+0x11c
x27: d936bc836a4a96bf   x20: d93715ba0000ffff   x25: 00000000000000b0 (=176)
FAR: 0036bc836a4a97df
栈回溯：newidle_balance → load_balance → find_busiest_group
```

（08-17 案例同 PC、同 CPU 179，索引为 175，进程 swapper/179，此处不赘述。）

### 4.2 指令链解码

```asm
+300: ldp x0, x1, [sp, #8]              ; 取局部变量
+308: ldr x20, [x0, w25, sxtw #3]       ; x20 = 数组[w25] —— 按 CPU 号索引的加载
+316: add x27, x1, x20                  ; 合成指针
+320: ldr x23, [x27, #288]              ; ← 崩溃（pc = +320 = +0x140）
```

### 4.3 寄存器 vs 内存对照

故障时 `sp = 0xffff8000b722b960` 未变，故 `[sp+8]` 仍保存数组基址：

```
crash> rd -64 ffff8000b722b968        → 0xffffa6c96a8955d0
crash> sym  ffffa6c96a8955d0          → __per_cpu_offset   （全局符号，精确命中）
crash> rd -64 ffffa6c96a895b50        → 元素[176]
```

dump 中 `__per_cpu_offset[172..177]` 为完美等距序列（步长 0x22000，per-cpu 区大小）：

```
idx172: ffffd93717256000   idx175: ffffd937172bc000
idx173: ffffd93717278000   idx176: ffffd937172de000  ← 代码本应读到的值
idx174: ffffd9371729a000   idx177: ffffd93717300000
```

而 CPU 179 实际载入的 x20 = `0xd93715ba0000ffff`：
高位片段 `d937` 与真值 `ffffd937…` 吻合，其余为垃圾——教科书式**撕裂读**（torn read）签名，指向缓存行/数据总线级的传输完整性缺陷，而非单比特翻转。

> 注：`[sp+16]` 处读出的 `0xffffa6c96a4996c0` 与崩溃寄存器 x1 完全一致，验证了栈解析的正确性。

---

## 五、根因判定

### 5.1 分层结论

| 层级 | 结论 |
|---|---|
| 直接致因 | writeback 合并检查解引用被污染的 `bv_page` → EL1 level-0 翻译错误 → Oops → kdump |
| 污染机制 | CPU 179 的加载指令返回撕裂/错误数据；其 MMU 页表遍历亦瞬时报假错误（72 次，4 次开机零例外） |
| **根因** | **CPU 179（MPIDR 0x7A0300，Socket/Node 7）硬件缺陷**——具体位于核私有缓存（L1/L2）、MMU/TLB 或为其服务的集群 L3 切片/互连端口，软件手段无法进一步细分，需厂商硬件诊断确认 |

### 5.2 置信度论证

支撑（按权重排序）：
1. 两例独立完成的"寄存器 vs 内存"对照实验，证明内存完好、读取出错；
2. 72 次 AT 复翻译成功的 spurious 告警，证明页表完好、遍历出错；
3. 异常 100% 与执行 CPU 相关（192 选 1，跨 4 次开机、13 天、多个互不相干子系统），与数据所在位置无关；
4. 崩溃形态跨子系统随机分布（writeback / 负载均衡 / procfs 读取 / COW 缺页），不符合任何单一软件 bug 的作用面；
5. 排除了热插拔、KASLR/kexec 伪影、单 DIMM 位翻转（见 5.3）。

保留的不确定性（如实声明）：
- 无法从 vmcore 排除"整机供电/时钟信号质量影响该核"这类平台级因素，其对软件的表现与核内缺陷相同，处置路径一致（厂商硬件检修）；
- 固件/SDEI（本机启用了 SDEI NMI watchdog）诱发的可能性无任何证据支持，但作为低概率残留项，随 BIOS/BMC 更新一并复核；
- OS 日志中无 GHES/APEI/EDAC 错误记录（固件优先模式下 corrected error 通常仅上报 BMC），因此缺少 BMC SEL 侧佐证，建议运维补查（见第七节）。

### 5.3 已排除假设及理由

| 假设 | 排除依据 |
|---|---|
| 内核 UAF/越界写破坏 bvec | 崩溃点内存内容完好无损；历史两例同样"内存好、读得坏"；三例分属互不相干子系统 |
| 外设 DMA 误写内存 | 同上（DMA 破坏必然持久留痕，与实验结果矛盾）；也无法解释页表遍历假错误 |
| 页表管理 bug / 内存热插拔 | dmesg 无热插拔事件；线性映射 PGD 自启动恒定；AT 重翻译每次成功 |
| 软件漏发 TLBI（陈旧转换） | 陈旧项只能提供有效旧翻译，不会造成 level-0 假错误；AT 走同一 TLB 也应命中 |
| irqbalance / procfs 自身 bug | 故障在内核数据拷贝；bash、调度器、writeback 三类无关路径同样出事 |
| 单 DIMM 位翻转/内存条故障 | 错误地址跨 NUMA 节点、跟随执行核而非物理地址；无单比特翻转特征；GHES 无记录 |
| kdump/KASLR 工具链伪影 | 告警分布于日常运行全程，与 kexec 动作无时间相关性 |
| 08-11 案例 | 为人工 `sysrq-c` 触发（comm=tee），非自发事件，不计入 |

---

## 六、影响面评估

- 该缺陷表现为**静默读损坏**：不仅导致可见崩溃，理论上也可能在不触发缺页的情况下把坏数据交给计算或写入磁盘（例如本次 writeback 所写文件数据、数据库页等）。
- ext4 元数据有校验和可拦截部分损伤；**文件数据内容不在保护范围内**。建议对关键业务数据做应用层校验/备份恢复演练。
- 系统在异常期间仍持续对外服务了 13 天，多数时间功能正常（故障为小概率瞬时事件），但崩溃风险与数据完整性风险并存。

## 七、处置建议

**立即（缓解）**
1. 下线 CPU 179：`echo 0 > /sys/devices/system/cpu/cpu179/online`（或经厂商指导隔离 Node 7 对应簇），观察告警是否归零。这是验证根因的最直接实验：若下线后 4 周内零复发，即可坐实。
2. 补查 BMC SEL 与传感器历史：`ipmitool sel list`、`ipmitool sensor`（重点：电压/温度越限、corrected error 计数、CPU 0x7A 簇对应记录），并将 GHES/EDAC 计数纳入监控（`/sys/devices/system/edac/mc/mc*/ce_count`）。

**短期（修复）**
3. 向服务器/CPU 厂商提交 RMA 申请，附本报告；现场更换顺序建议：先 CPU（MPIDR 0x7A0300 所属芯片），再主板/底座。厂商可用 MPIDR 与 NUMA 拓扑精确定位封装单元。
4. 更换前后各跑一轮厂商内存/CPU 诊断（如 HiSToL/memtester 压测 Node 7）。
5. 复核 BIOS 7.48 (2026-06-15) 与 BMC 固件版本说明，若有更新按厂商建议升级（低概率相关项：SDEI watchdog 行为）。

**长期（防御）**
6. 对同类大规格 ARM 服务器部署 GHES/EDAC 与 SEL 的周期采集告警，缩短此类慢性故障的发现周期（本案从首发告警到致命崩溃间隔 6 天以上，具备提前干预窗口）。
7. 将 `/proc/interrupts` 高频读取方（irqbalance 等）触发的 spurious 告警纳入主机健康巡检规则：**该告警在本环境应被视为硬件故障的前兆信号，而非噪音**。

## 八、附录

### A. 分析工具与方法
- crash 8.0.4-17.oe2403sp3 + `kernel-debuginfo-6.6.0-145.3.23.154.oe2403sp3.aarch64`（与转储精确同版本，含 debugsource 用于源码核对）
- 关键命令：`bt` / `dis bio_add_page` / `struct bio|bio_vec|page <addr>` / `rd -64 <addr>` / `sym <addr>` / `p __cpu_logical_map[179]` / `vtop`
- 反汇编核对：gdb `disassemble /r find_busiest_group`；源码核对：`/usr/src/debug/.../arch/arm64/mm/fault.c`

### B. 关键地址换算
- VA_BITS=48：线性映射基址 `0xffff000000000000`，physvirt_offset=`0x0001000000000000`；vmalloc `[0xffff800080000000, 0xfffffbffefffffff]`；vmemmap `[0xfffffc0000000000, …]`
- 本报告全部 VA→PA 均按 `pa = va + 0x0001000000000000 (mod 2^64)` 计算

### C. 本次开机 34 次 spurious 告警的物理地址归属

| NUMA 节点 | 次数 | 示例（t 秒 → PA） |
|---|---|---|
| Node 7 | 25 | 835s→`0x604005eb5395`；16361/16543/18323s 反复命中 `0x604006ed9xxx` 同一页 |
| Node 0 | 4 | 11447s→`0x2030faf4f5` |
| Node 4 | 3 | 537455s 三连击同一页 `0x40295ce62xxx` |
| Node 3 | 1 | 35785s→`0x204009f1675d` |
| vmalloc | 1 | 6940s→bash COW 路径 `0xfffc360a9e44e08` |

### D. 证据文件清单
- 本目录：`vmcore`、`vmcore-dmesg.txt`、本报告
- 佐证转储：`../127.0.0.1-2026-08-14-19:07:04/`（完整 vmcore，已完成对照实验）；`../127.0.0.1-2026-08-17-13:47:08/`（vmcore-incomplete，仅 dmesg 可用）

---
*分析人：ox-alpha（Claude Code 环境）· 2026-08-25 · 全部结论均有可复现命令输出支撑；无法从现有数据进一步收窄的部分已在文中如实标注。*
