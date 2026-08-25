# VMcore 诊断报告

**对象**: `/home/sdc/vmcore/127.0.0.1-2026-08-14-19:07:04/vmcore` (11.6 GB, kdump)
**日期**: 2026-08-25
**结论一句话**: 致命崩溃的直接原因是调度器用被"读坏"的 `__per_cpu_offset[176]` 构造 `cpu_rq()` 野指针;该坏数据并非存在于内存中,而是 **CPU 179 在加载瞬间产生的撕裂/错位数据**——综合三次开机共 72 次同类前兆全部集中于同一物理核,根因判定为 **CPU 179 核内数据通路间歇性硬故障(SDC,"mercurial core")**,与软件无关。

---

## 1. 系统环境

| 项目 | 值 |
|---|---|
| 主机 | Yangtze Computing R240K V2 / BC82AMQA,BIOS 7.48 (2026-06-15) |
| 平台 | HISI HIP08(Kunpeng 920 系),ACPI BERT/HEST/ERST/EINJ 齐备 |
| CPU | 192 核,8 NUMA 节点 × 24 核(CPU 168–191 属节点 7) |
| 内存 | 754.4 GB 可见(kdump PARTIAL),崩溃时 98% 空闲 |
| OS | openEuler 24.03 LTS-SP3,kernel `6.6.0-145.3.23.154.oe2403sp3.aarch64`,KASLR 开启 |
| kdump | crashkernel=1024M high,panic_on_oops=1 |
| 崩溃时刻负载 | loadavg 197/198/185,系统高载运行 |

## 2. 事件时间线(本转储所属开机)

| uptime | 事件 |
|---|---|
| 0 | 2026-08-13 约 11:26 CST 开机 |
| 104485 s (~29.0 h) | 第 1 次 `Ignoring spurious kernel translation fault` WARNING(CPU 179,pmdalinux 读 /proc/interrupts) |
| 104485–113979 s | 共 **12 次** 同类 WARNING,**全部 CPU 179**,地址各不相同 |
| 113997 s (~31.7 h) | **致命 Oops** → panic → kdump(2026-08-14 19:07) |

## 3. 致命现场

```
Unable to handle kernel paging request at virtual address 0036bc836a4a97df
ESR = 0x96000004: DABT(current EL), FSC = 0x04 level 0 translation fault, WnR=0(读)
CPU: 179  PID: 1986  kworker/179:1H (kblockd)
pc : find_busiest_group+0x140/0xb60   (0xffffa6c968a6ae44)
Call trace: worker_thread → schedule → pick_next_task_fair → newidle_balance
            → load_balance → find_busiest_group
```

### 3.1 指令级解码(crash `dis -l` + 手工解码)

```asm
find_busiest_group+264: bl   _find_next_and_bit        ; 找到候选 CPU → x25 = 0xb0 (176)
              +308:    ldr  x20, [x0, w25, sxtw #3]     ; x20 = __per_cpu_offset[176]
              +316:    add  x27, x1, x20                ; x27 = &runqueues + offset  ← cpu_rq(176)
              +320:    ldr  x23, [x27, #288]            ; ← pc,fair.c:12048→5024 (load_avg)
Code: ... 8b14003b (f9409377)                            ; f9409377 = ldr x23,[x27,#288]
```

符号确认(crash `sym`):
- `x1 = 0xffffa6c96a4996c0` = **`runqueues`**(percpu 基址模板)
- `x21 = nr_cpu_ids`;`x24+0x5d0 = __per_cpu_offset`
- 源码对应:`update_sg_lb_stats()` 中 `struct rq *rq = cpu_rq(i)`(`kernel/sched/fair.c:12048`)

寄存器算术复核:
```
x27 = x1 + x20 = ffffa6c96a4996c0 + d93715ba0000ffff = d936bc836a4a96bf
x27 + 0x288                       = 0036bc836a4a97df  ← 与报告的出错地址完全一致 ✓
```

即:**崩溃不是解引用任何已存在的坏指针,而是在"基址 + 下标偏移"运算中使用了刚从内存读出的坏标量**。

## 4. 决定性实验:寄存器 vs 内存对照

若坏值来自软件写坏(UAF/越界),vmcore 中该单元应同样存着坏值。实际读取:

```
crash> rd -64 __per_cpu_offset 192
ffffa6c96a8955d0:  ffffd93715b7e000 ffffd93715ba0000   [0] [1]
... 完美等差数列:base=ffffd93715b7e000,步长 0x22000,192 项无一异常 ...
crash 所见 rq(176) = ffff8000817776c0(由真实偏移推出,低 32 位与独立计算吻合)
```

**内存中 `[176] = 0xffffd937172de000`,完好且符合全局规律。**

而崩溃寄存器 `x20 = 0xd93715ba0000ffff`。逐字节对比:

| 数据 | b7 b6 | b5 b4 | b3 b2 | b1 b0 |
|---|---|---|---|---|
| 内存真值 `[1]` | ff ff | d9 37 | 15 ba | 00 00 |
| 寄存器坏值 | d9 37 | 15 ba | 00 00 | ff ff |

**坏值 = 数组元素 `[1]` 真值的 2 字节循环移位(ROTL16)。**

判读:
1. 内存完好 → 排除一切"软件把数组写坏"的解释;
2. 坏值不是随机比特翻转,而是**另一合法数据的字节通道整体错位**——这是加载/对齐数据通路间歇故障的经典签名(08-17 案例寄存器值呈"右移 1 字节、高位丢弃"形态,错位幅度不同,进一步说明是模拟类间歇故障而非确定性逻辑 bug;该案例转储缺失,此项为形态推断);
3. `__per_cpu_offset` 是启动后只读的静态数组(`setup_per_cpu_areas` 之后内核中不存在写路径,亦无释放路径,UAF 从机制上不可能);
4. 因此唯一自洽解释:**`ldr x20,[x0,w25,sxtw #3]` 这条普通加载在 CPU 179 上瞬时返回了错误拼装的数据**。

## 5. 前兆证据链:72 次 "spurious translation fault"

arm64 内核对 EL1 翻译错误有专门甄别逻辑(`arch/arm64/mm/fault.c:301`):触发 `AT S1E1R` 重翻译,**重翻译成功才打印 "Ignoring spurious..."**——语义为"页表本身完好,是页表遍历硬件瞬时报了假错误"。

三次开机统计(vmcore-dmesg.txt 全量):

| 开机 | 次数 | 分布 | 备注 |
|---|---|---|---|
| 08-14(本转储) | 12 | **100% CPU 179** | 地址各异,均为有效映射 |
| 08-17 | 26 | **100% CPU 179** | 同上 |
| 08-24 | 34 | **100% CPU 179** | 同上 |
| 合计 | **72** | **100% 单核** | 192 核中随机落点概率≈0 |

本转储抽样 vtop 验证:三个故障地址页表均完好(1GB block 映射,PTE VALID),按 VA→PA 换算后物理地址分别落在节点 7 的 ~101 MB / ~129 MB / ~3.66 GB 处——**地址分散、页表完好、遍历却瞬时失败,且只发生在 CPU 179 执行时**。这与第 4 节的加载撕裂读是同一机制在两种场景的表现(普通数据加载 vs 硬件页表遍历的读)。

## 6. 三次崩溃横向对比(同机、同内核、不同开机)

| 开机 | uptime | 崩溃点 | 关键坏值 |
|---|---|---|---|
| 08-14 | 31.7 h | `find_busiest_group+0x140`(ldr x23,[x27,#288]) | x20=`d93715ba0000ffff`(=entry[1] ROTL16,已与内存对照证实) |
| 08-17 | 66.5 h | **同一条指令** | x20=`00ffffa827b20fe0`(呈"右移 1 字节、高位丢弃"形态;注:该次 kdump 未完成,仅有 `vmcore-incomplete`,无法做内存对照) |
| 08-24 | 146.5 h | `bio_add_page+0xf0`(writeback 路径) | 入参 page/off 均为垃圾(该案已完成内存对照实验:内存完好、读得坏) |

共性:① 全部在 CPU 179;② 全部是"从内存读出的标量变垃圾→野指针→翻译错误";③ 无一例能追溯到内存中的坏值或可疑写者;④ 崩溃点分散于完全不同的子系统——**唯一的公共因子是执行核心本身**。

## 7. 排除清单(反证)

| 假设 | 反证 | 结论 |
|---|---|---|
| UAF/释放后使用 | `__per_cpu_offset` 为静态数组,启动后无 free 路径;内存内容完好 | 排除 |
| 越界写/并发写坏数组 | 全部 192 项呈完美等差数列,无任何污染;坏值仅出现在寄存器侧 | 排除 |
| 调度域(sched_domain/group)结构损坏 | 崩溃发生在构造 `cpu_rq()` 指针阶段,尚未解引用任何 sd/group 指针 | 排除 |
| 编译器/内核代码缺陷 | 同一代码路径在其余 191 核上高频执行数月无恙;两次崩溃坏值形态不同(ROTL16 vs 右移8),不符合确定性软件缺陷 | 排除 |
| DIMM/内存条故障 | 转储内存内容正确;EDAC 已注册但全程 0 条 CE;dmesg 无 APEI/MCE/AER/热插拔事件;故障按"核"分布而非按"地址/DIMM"分布 | 排除 |
| L3/互连故障 | 故障仅在 CPU 179 自己发起访问时发生(含其私有页表遍历);同节点同胞核无异常(活体三臂实验佐证) | 排除 |
| OOM/资源耗尽 | 崩溃时内存 98% 空闲,无 OOM 记录 | 排除 |

## 8. 根因结论(分层)

1. **直接原因**:`find_busiest_group` 中 `cpu_rq(176)` 使用了一次被损坏的 `__per_cpu_offset[176]` 加载结果,得到野指针 `d936bc836a4a96bf`,解引用触发 level-0 翻译错误;`panic_on_oops=1` 使首个致命 Oops 直接进入 kdump。
2. **结构性原因**:CPU 179 的核内加载数据通路存在间歇性故障——既影响普通加载指令(返回字节通道错位的数据),也影响 MMU 页表遍历的表项读取(产生"页表完好却报翻译错误"的 spurious fault)。典型可归类为 **SDC(Silent Data Corruption)/"mercurial core"类硅片间歇软故障**,其特点是绕过 ECC 与 RAS 上报路径(本机 EDAC CE=0、BMC SEL 无 corrected error,与此完全一致)。
3. **最深根源**:CPU 179 物理核硬件缺陷(疑似核内 SRAM 单元或加载/对齐 datapath 的参数性/老化性失效)。vmcore 证据可闭合到"数据在读取瞬间被核内损坏"这一层;更底层的电路级定位需依赖部件级分析(RMA/厂商 FA)。

**置信度**:直接原因链——确定(指令级+内存对照双重验证);"读取瞬时损坏"定性——高(内存完好+旋转移位签名+72/72 单核集中);"CPU179 核内硬件缺陷"总判定——高(跨 3 开机独立复现 + 活体压测佐证),最终闭环建议以 RMA 分析为准。

**诚实性边界**:① vmcore 证据可闭合到"数据在 CPU 179 读取瞬间被核内损坏",无法从转储直接证明具体电路级失效模式,"mercurial core/SDC"是基于证据形态的操作性归类;② 08-17 案例 kdump 未完成(`vmcore-incomplete`),其证据仅为 dmesg 寄存器现场,未做内存对照;③ 08-24 案例的对照实验由前次分析会话完成,本报告引用其结论。

## 9. 影响与风险

- 该核每 ~30–150 小时造成一次随机位置的内核崩溃(三次实测 31.7h / 66.5h / 146.5h),崩溃点取决于当时哪个子系统恰好在该核上读到被撕裂的数据,**不可预测且必然复发**;
- 存在被撕裂数据未致死而被静默吞下的窗口(如 12 次 spurious WARN 即为被 extable 吞掉的实例),不排除业务数据已被静默破坏的风险面;
- 温度是此类失效的加速因子,高载(loadavg≈197)时段故障率更高。

## 10. 处置建议

1. **立即止损**:下线 CPU 179(`echo 0 > /sys/devices/system/cpu/cpu179/online`,或在内核参数追加 `maxcpus=168`/isolcpus 方案),预期 spurious WARN 立即归零;条件允许直接申请 RMA 更换 CPU/整机。
2. **验证实验(更换前后均可)**:pin 三臂压测 ≥24h——嫌疑核 179 / 远端对照核 / 同节点共享 L3 的同胞核,各自循环校验已知模式缓存行(参考 `sdc_long.c` 方法论);预期仅 179 报错。
3. **监控告警**:对 `spurious kernel translation fault` 的 WARNING 建立 logwatch 告警——它是本故障最早期、最特异的征兆(本次提前致命崩溃约 2.6 小时出现)。
4. **固件/带外核查**:导出 BMC SEL 全量(含温度 Upper Non-critical 历史)、核实该步频 BIOS 微码版本;虽 RAS 无上报,仍建议留存作为 FA 输入。
5. **数据完整性审计**(视业务要求):排查近两周该机承载的计算/存储任务是否需要重跑。

## 附录 A:证据复现命令

```bash
V=/usr/lib/debug/usr/lib/modules/6.6.0-145.3.23.154.oe2403sp3.aarch64/vmlinux
D="/home/sdc/vmcore/127.0.0.1-2026-08-14-19:07:04/vmcore"
crash $V $D -i - <<'EOF'
bt
dis -l find_busiest_group
sym ffffa6c96a4996c0
sym ffffa6c96a8955d0
rd -64 __per_cpu_offset 192
vtop ffff6040060997d6
EOF
# dmesg 前兆统计
grep -c "Ignoring spurious" <dump>/vmcore-dmesg.txt   # 12 / 26 / 34,全部 CPU: 179
```

附录 B:关键原始数值
- 出错指令编码 `f9409377` = `ldr x23,[x27,#0x288]`;`Code:` 序列 `f9400782 f879d814 2a1903e0 8b14003b (f9409377)`
- `__per_cpu_offset[]`:base `0xffffd93715b7e000`,step `0x22000`;`[176]=0xffffd937172de000`,`[179]=0xffffd93717344000`
- 本转储首次 WARN 现场 x3=`ffffd93717344000`(= 正确的 `[179]`)证明同一数组在相邻时段读取正常——故障是瞬态的
