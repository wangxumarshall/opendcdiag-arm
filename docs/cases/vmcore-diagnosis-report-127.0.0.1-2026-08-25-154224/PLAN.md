# 诊断计划 — vmcore 127.0.0.1-2026-08-25-15:42:24

## 已知现场（勘察结论，先于分析）
- 转储完整（27.6 GB），内核 `6.6.0-145.3.23.154.oe2403sp3.aarch64`，Kunpeng-920/HIP08。
- panic：CPU179, PID2018836(claude), PC=`find_busiest_group+0x140/0xb60`，
  ESR=0x96000007（EC=0x25 DABT current EL / FSC=0x07 L3 translation fault / WnR=0 读），
  故障 VA=`ffffa5aa9b5a97e0`，页表走到 pte=0。
- 同机历史：08-14、08-24 两次崩溃同为 CPU179、同为 find_busiest_group+0x140，
  前案已建立"寄存器 vs 内存对照 + 撕裂读比特分析"方法论并判为核内 L1d SDC(mercurial core)。
- 本次开机期间有 l1d_disable 实验（SCTLR_EL1.C 开关）与 opendcdiag 运行记录；崩溃时已卸载。
- **16 分钟后机器再次崩溃**（15:58:09 目录），必须纳入证据链。

## 执行原则
1. 从零独立验证，不因前案预设结论——本次若证据不支持硬件根因，必须如实改判。
2. 每个结论给出可复核的命令与输出摘录；区分"事实/推论/假设"三级置信标注。
3. 软件根因（UAF、域重建竞态、内核 bug）必须被正面排除而非默认排除。

## 阶段
### P1 勘察与准备
- [ ] debuginfo 与 vmlinux 就位校验
- [ ] 15:58:09 新转储 dmesg 尾部（是否第四次同型崩溃）
- [ ] 目标转储 dmesg 中 spurious fault 统计、sched/hotplug/RAS 相关事件

### P2 静态定位崩溃指令（不依赖 crash，objdump 可先行）
- [ ] 反汇编 find_busiest_group+0x140 处指令，确认 faulting load 的目标寄存器/偏移
- [ ] addr2line 映射到 kernel 源码精确行（fair.c）
- [ ] 回溯 x27/x1 的数据流：该指针由哪条 load/哪个结构体字段提供

### P3 crash 动态取证（批量命令文件，sudo 后台）
- [ ] sys/bt/寄存器全量确认（vmcore 与 dmesg 一致性）
- [ ] 崩溃任务上下文（rq、sd 层级）、调度域内存完整性遍历
- [ ] 决定性实验：x27 来源字段的"内存实际值"vs"寄存器读到的值"对照
- [ ] 若命中损坏：16-bit 半字/字节级比对，判断撕裂签名 or 随机损坏 or 内存持久损坏

### P4 软件根因正面排除
- [ ] sched domain 生命周期事件（hotplug/rebuild/隔离参数）
- [ ] UAF 可能性：故障页所属 vmalloc 区间属性、对象归属判定
- [ ] 该内核版本 find_busiest_group 已知缺陷检索
- [ ] RAS/EDAC/BERT/SEL 上报情况（负证据链）

### P5 跨开机统计与结论
- [ ] 全部转储的 per-CPU 异常分布
- [ ] 三/四次崩溃的同点复现显著性
- [ ] 结论置信度声明 + 处置建议（下线 CPU179 / RMA / 监控）

### P6 报告交付
- DIAGNOSIS_REPORT.md 写入转储目录（/tmp 撰写 → sudo cp）

## 置信度约定
【实锤】= dump 内可复核证据；【强推】= 多源证据收敛的推断；【假设】= 无法软件验证的部分，明示验证途径。
