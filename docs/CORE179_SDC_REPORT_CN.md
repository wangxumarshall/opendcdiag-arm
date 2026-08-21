# Core 179 SDC 缺陷——完整测试报告(中文)

**日期**:2026-08-20
**状态**:跨路径确证完成,位翻转规律量化,缺陷定位到 load/store 通路
**目标机器**:192 核 aarch64(8 NUMA 节点 × 24 核),Linux 6.6.0-145,package 19062
**测试框架**:`opendcdiag`(meson+ninja,`builddir/opendcdiag`),Eigen 3.4.0
**触发配置**:全核心(无 taskset)+ 自动 seed(无 `-s`)+ `--max-test-loop-count=0`(seed 在窗口内固定)+ `-t 15m`

---

## 一、缺陷概述

在 192 核 aarch64 机器的**逻辑 core 179**(NUMA 节点 7,module 23340)上存在一个静默数据损坏(SDC)缺陷。

**核心特征**:
- **单核**:所有失败都落在 cpu-mask 第 4 段第 36 位(全局 bit 179),从不漂移到其他 191 个核。
- **多 bit 破坏**:每次失败的 xor(golden ^ actual)翻转 10–39 个 bit,无一例单 bit。排除单 bit SEU(宇宙射线)。
- **概率性**:同一 seed 可能这次失败、重试通过;翻转的 bit 每次不同,无固定位。
- **需多核并发压力**:单核独占 179(`taskset -c 179`)或固定单一 seed 都不触发;必须全核心并发 + 自动 seed。
- **跨指令路径**:不是某条指令的 bug——movbe、mrn_rmw、eigen_gemm 全系列、eigen_svd 都触发,缺陷在 core 179 的**公共 load/store 通路**。

**翻转位置规律**(36 样本/562 bit 统计):翻转集中在**尾数区**(float 85%、double 93%),**符号位几乎免疫**(0–1/562),翻转 bit 数与计算路径强相关(SVD 多为单 bit、GEMM 多 bit 高密度)。

---

## 二、测试环境与触发条件

### 2.1 机器
- 192 核,8 NUMA × 24 核,package 19062
- core 179 = NUMA 7,module 23340,thread 0

### 2.2 触发配置(必须同时满足)
| 条件 | 值 | 原因 |
|------|-----|------|
| 核心数 | 全 192 核(无 taskset) | 多核并发压力抬高触发率;单核独占 179 = 0 fail |
| seed | 自动(不传 `-s`) | 不能固定 LCG seed;固定 seed = 单一输入模式,会误判 |
| `--max-test-loop-count=0` | seed 在时间窗口内固定 | 禁用 fracturing(seed 轮换),让单一模式持续暴露,触发率最高 |
| 时长 | `-t 15m` | 给足时间窗口 |

### 2.3 Eigen 3.4.0 重编(重要)
原版 `eigen_svd` / `eigen_svd_cdouble_noavx512` 使用 BDCSVD 算法,在系统自带 Eigen 3.3.8 + gcc `-std=c++23` 下**编译失败**:
```
BDCSVD.h:355: error: return type of operator!= is not 'bool'
note: used as rewritten candidate
```
根因:Eigen 3.3.8 的 BDCSVD 源码 `A.col(j).array()!=Literal(0)` 在 C++23 严格的重写候选规则下非法。**解法**:把 meson 的 `eigen3_root` 指向 `/usr/local`(Eigen 3.4.0,已修复此 bug),原版 svd 即可编译运行。

---

## 三、各测试用例的负载类型与翻转规律

本节涵盖**所有**参与 SDC 调查的测试用例,逐个说明负载类型、翻转规律、是否触发。

### 3.1 movbe / movbe_dump —— 整数字节交换 + store→reload

**负载类型**:整数字节交换,带**同缓冲区 store→reload** 模式。源码 `tests/cpu/misc/movbe.cpp` / `movbe_dump.cpp`。64 KB 的 `uint32_t` 缓冲(16384 字)在 init 时用 `random32()` 填充,每次迭代复用。每个元素的热循环:
```
val = data->input[i];          // 第1次读
val = __builtin_bswap32(val);  // 字节交换(rev)——不参与比较
data->swapped[i] = val;        // 存到不同 cache line(swapped[])
val = __builtin_bswap32(val);  // 第2次交换→编译器折叠为 input[i] 的 RELOAD
if (val != data->input[i])    // 捕获 reload != 第1次读
```
翻转点在 **reload `ldr` `data->input[i]`**(第 4 条指令)。正确核返回刚读的值,缺陷 core 179 间歇返回不同值。足迹 64 KB(跨 cache line,同 LLC domain)。这是所有负载里**最紧密、相位最稳定**的 store→load 序列,所以 movbe 触发最频繁(约 0.24 events/min)。

**翻转规律——多 bit,无固定位,无符号/量级规则**。15 个 movbe_dump 样本(seed 1711090087 的 11 个 + 5x 跑的 4 个):

| 跑次 | seed | ttf(ms) | xor | 翻转 bit 数 |
|------|------|---------|-----|------------|
| repro1 | LCG:1711090087 | 10712 | 0x0A11CC33 | 12 |
| repro2 | LCG:1711090087 | 6882 | 0x0446C470 | 10 |
| repro3 | LCG:1711090087 | 38317 | 0x59B2328C | 14 |
| repro4 | LCG:1711090087 | 27044 | 0x69A6841F | 15 |
| repro5 | LCG:1711090087 | 29502 | 0xA5E5AABF | 20 |
| repro6 | LCG:1711090087 | 101852 | 0xEF9545ED | 20 |
| repro7 | LCG:1711090087 | 151023 | 0x691B1AB0 | 14 |
| repro8 | LCG:1711090087 | 7145 | 0x9264DAA3 | 15 |
| repro9 | LCG:1711090087 | 20953 | 0x72D6417B | 17 |
| repro10 | LCG:1711090087 | 65875 | 0x035ABA5E | 16 |
| repro11 | LCG:1711090087 | 249629 | 0xCC6D1577 | 18 |
| 5x run2 | LCG:1072812489 | 1893 | 0x5AEF73F2 | 21 |
| 5x run3 | LCG:1047109210 | 1626 | 0xB57CB778 | 20 |
| 5x run4 | LCG:763356360 | 1657 | 0xD1B0A9CD | 16 |
| 5x run5 | LCG:1314873594 | 271 | 0x2B91F058 | 14 |

全部多 bit(10–21),xor 每次不同(无固定位),bit 散布整个 32 位字,无字段偏好(movbe 是整数路径,IEEE754 区域分析不适用)。**规律性**:bit 模式本身无规律(每次 fail 的 xor 实际随机),但统计特征强一致——总是多 bit、从不单 bit、总在 179。

### 3.2 mrn_rmw / mrn_rmw_dump —— 整数 RMW + store-to-load 转发

**负载类型**:整数 ALU(add/sub/xor/and,按 `i%4` 轮换)+ **显式同地址 store→load 转发**链。源码 `tests/cpu/misc/mrn_rmw.cpp`。两个 8 KB 源数组(`srcA`、`srcB`,各 1024 个 `uint64_t`)用 `memset_random` 填充;golden `expected[]` 预计算。热循环:
```
a = srcA[i]; b = srcB[i];
res = (i%4==0)?a+b : (1)?a-b : (2)?a^b : a&b;
store_qword(&temp[i], res);               // str
store_qword(&dst[i], load_qword(&temp[i]));  // ldr 刚存的 temp[i] → 转发
...
memcmp(dst, expected, ...)                 // 批量校验
```
aarch64 上 store/load 用内联汇编 `str`/`ldr` + `"memory"` clobber,强制保持为**同地址**(`temp[i]`)相邻的 `str`→`ldr`。这是 movbe 的**最近结构类比**:存后立即取刚写位置(store-to-load forwarding 过 store buffer)。足迹 8 KB(跨线)。ALU 依赖链。

**翻转规律**。历史 `all_tests_2h` 记录 2 个 fail 但**无值 dump**(原版只 `report_fail_msg("mrn_rmw data miscompare")`——只记消息+源行,无 actual/expected/xor)。dump 变体战役(`mrn_rmw_dump`)捕到**一个**完整样本:idx 782,op=xor,golden=0x6740051755D4E720,actual=0xB9D09DCDF55B98CC,**xor=0xDE9098DAA08F7FEC → 35/64 bit 翻转**。这是数据集里**最高密度**翻转(54% bit),高位密集破坏,散布整个 64 位字。与 movbe 的多 bit 特征一致;单样本太薄不能定模式,但绝不是单 bit。

### 3.3 memcpy1 —— 纯内存拷贝(无 ALU)——⚠ 从未触发

**负载类型**:**纯 memcpy + 立即校验**——唯一无任何算术的负载。源码 `tests/cpu/memory/memcpy1.cpp`。256 字节 `src` 块(栈上)每迭代用 `std::mt19937` 填随机字节,`memcpy` 到 `dst`,再 `memcmp(dst,src,256)` 校验。每迭代是 **load(src)→store(dst)→load(dst 做 memcmp)** 往返,256 字节,密集的 store→load 对,无 ALU 依赖。

**翻转规律——从未触发(更正)**。原版 `memcpy1` 只 `report_fail_msg("memcpy1: data mismatch after copy")`,无值 dump。

⚠ **重要更正**:此前报告中"memcpy1 历史 6 fail on 179"是**解析错误**。用 byte-offset 精确定位(fail 前最近的 `- test:` 行)核实:两个 `all_cores_10m` 日志里的 6 个 fail **全是 movbe**(seed LCG:163174881 ×4 + LCG:1410093522 ×2),不是 memcpy1。`all_tests_2h` 里 496 个 memcpy1 分片**全部 pass**。

专门重测也确认 memcpy1 单测不触发:
- 5min + `--max-test-loop-count=0`(seed 固定):0 fail
- 10min + 默认 fracturing(轮换 11180 个不同 seed):0 fail

两次都全 192 核跑满(stderr 消息显示多 worker 交错),但全 PASS。结论:**memcpy1 从未触发 SDC**,它不是 SDC 触发负载。256 B 小足迹的 store→load 序列单负载下相位太不稳定,不足以命中 core 179 的触发窗口。

### 3.4 eigen_gemm_float_dynamic_square —— float FMA GEMM

**负载类型**:**FMA 矩阵乘**,单精度。源码 `tests/cpu/eigen_gemm/gemm_float_dynamic_square.cpp`。init 时生成两个 256×256 `float` 随机矩阵 `lhs`、`rhs`,golden `prod=lhs*rhs` 算一次。热循环重算 `x=lhs*rhs` 并做**字节精确** `memcmp_or_fail(x.data(), prod.data(), 256*256)`(无容差,一个尾数 bit 翻转就 fail)。底层 Eigen 发出大量 `fmadd`/`fmla` 微操作,数百 KB 矩阵数据;store/load 往返发生在结果写回(`x.data()` 写 → `memcmp_or_fail` 读)。足迹数百 KB。

**翻转规律——尾数集中、多 bit、符号免疫**。11 个历史样本(30min stress,seed LCG:1756755722)+ 1 个 dump 战役样本([0,20])。代表性 xor(float 32 位):

| 位置 | mask | 翻转 bit | 字段 |
|------|------|---------|------|
| dump [0,20] | 0x0001F1F0 | 10 | 尾数+低位指数 |
| 历史 | 0x7FE873E6 | 20 | 符号+指数+尾数(罕见符号命中) |
| 历史 | 0x000506EC | 8 | 尾数 |
| 历史 | 0x01D68734 | 11 | 指数+尾数 |
| 历史 | 0x001DF0CC | 11 | 尾数 |

全部 12 个 float 样本:**尾数 85%、指数 14%、符号 1%**(仅 0x7FE8… 命中符号)。翻转 bit 数中位 12(范围 6–21)。**规律**:破坏落在尾数(精度 bit),避开符号位,多 bit——与 cache line/结果缓冲破坏一致,不像 FMA 语义错(那会系统性指数错)。

### 3.5 eigen_gemm_double_dynamic_square —— double FMA GEMM

**负载类型**:同 3.4,但**双精度**(256×256 `double`,`Matrix<double,Dynamic,Dynamic>`),golden 预计算,字节精确 `memcmp_or_fail`。FMA 流更宽(64 位累加器),足迹约 512 KB。同样写回→校验 store/load 往返。

**翻转规律——所有路径中最密的多 bit**。11 个历史样本(seed LCG:546398148)+ 1 个 dump 战役样本([0,92])。代表性 xor(double 64 位):

| 位置 | mask | 翻转 bit |
|------|------|---------|
| dump [0,92] | 0x000081FD12D509E0 | 21 |
| 历史 | 0x0002DECCDDA05EB9 | 25 |
| 历史 | 0x7FFCDE1DBC270D91 | 38 |
| 历史 | 0x000053DF26BFED9D | 21 |
| 历史 | 0x006484561086551E | 28 |

全部 12 个 double 样本:**尾数 93%、指数 6%、符号 0%**。翻转 bit 数中位 28(范围 20–39)——**所有路径最高最密**。规律同 float(尾数、符号免疫、多 bit)但每次翻转更多 bit,随尾数字段更宽(52 vs 23 bit,落点更多)。0x7FFC…/0x7FFD… 命中指数高位但从不碰符号位。

### 3.6 eigen_gemm_double14 —— double GEMM + 额外 copy/verify 往返

**负载类型**:double GEMM(256×256,M_DIM=256)**每迭代额外 copy→verify 往返**(`tests/cpu/eigen_gemm/double14.cpp`)。每迭代:copy `_x=lhs`、`_y=rhs`,算 `_prod=_x*_y`,然后**三对** isApprox + memcmp_or_fail 校验(`_x`/`_y`/`_prod` vs 原值)。比普通 dynamic_square 多了 store→load 往返(copy-in + verify-out),所以触发概率更高。

**翻转规律——触发但无位翻转数据**。double14 的失败(30min stress 10 fail + 战役 3/5)全命中 `_prod.isApprox failed`(double14.cpp:76)。因为 `isApprox` 是**浮点容差比较**,在字节精确 `memcmp_or_fail`(第 78 行)之前就 fire,所以框架的 `data-miscompare` 块(含 actual/expected/mask)**永远到不了**。日志只记 `E> Failed at .../double14.cpp:76: _prod.isApprox failed` + cpu-mask + seed。因此 double14 贡献**触发确认**(它是最频繁的 double 路径触发器)但**零位翻转位置数据**。翻转规律:无法确定(isApprox 掩盖了 bit);触发规律:179 上强(10+3 fail)。

### 3.7 eigen_gemm_cdouble_dynamic_square —— 复数 double FMA GEMM

**负载类型**:`std::complex<double>` 221×221 矩阵的 FMA GEMM(M_DIM=221,"故意怪维度")。golden 预计算;字节精确 `memcmp_or_fail` 对 reinterpret 的 `double*`(2×221×221=97641 个 double)。复数 GEMM 底层分解为 4 个实 GEMM(real×real、real×imag、imag×real、imag×imag)。

**翻转规律——低率路径,但确实触发(v3 重测)**。三个数据点:
- 历史 `all_tests_2h`:1 fail,mask=0x33CEC39BE4109264(28 bit double 路径翻转)。
- 8×15min dump 战役:**0 fail**(cdouble 和 cdouble_dump 各 15min)——窗口太薄。
- **v3 重测(Eigen 3.4.0 重编,15min,全核心,自动 seed,`--max-test-loop-count=0`):1 fail on core 179**,ttf=94856ms,seed LCG:874481998,mask=**0x0000BA899C502FDC → 24 bit 翻转**(bit 2-47,尾数+低位指数)。这确认 cdouble **能触发**——它是最低率的 eigen 路径,单次 15min/1-seed 窗口是抛硬币,不是"不能触发"。24 bit 翻转与 §3.5 的 double GEMM 特征一致(多 bit 整体破坏),证明缺陷与实数/复数算术路径无关,是同一个。

### 3.8 eigen_svd —— 迭代 BDCSVD(float)——以单 bit 为主

**负载类型**:**迭代奇异值分解**,源码 `tests/cpu/eigen_svd/svd.cpp`(float,M_DIM=256)。用 Eigen 的 `BDCSVD`(分治双对角化)——**迭代**算法:大量矩阵乘 + Householder 反射 + 扫描,反复 load/store 中间矩阵。golden = 首线程的 U/V 矩阵;后续迭代字节精确 `memcmp_or_fail` 比较(经 `compare_or_fail`)。足迹数百 KB,但访问模式是**迭代驱动**(跨扫描读改写),不像 movbe/mrn_rmw 的紧密 store→reload。

> 注:原版 eigen_svd 用 Eigen 3.4.0 重编后可在当前 binary 运行(见 §2.3)。

**翻转规律——所有路径中唯一以单 bit 为主**。11 个 float SVD 样本(seed LCG:526650671)+ 1 个 cdouble SVD 样本(LCG:576148252)。float SVD xor:

| mask | 翻转 bit |
|------|---------|
| 0x0000000E | 3 |
| 0x00000001 | 1 |
| 0x000001FC | 7 |
| 0x00000001 | 1 |
| 0x00003448 | 6 |
| 0xE34FEFCC | 21 |
| 0x00000001 | 1 |
| 0x00000004 | 1 |
| 0x0000000E | 3 |
| 0x7656F3B0 | 17 |
| 0x00000002 | 1 |

翻转 bit 数中位 **3**,**5/11 样本恰好 1 bit**。这是唯一以单 bit 为主的路径——且单 bit 翻转集中在尾数最底位(bit 0/1/2,mask 0x1/0x2/0x4/0xE)。**规律**:SVD 的迭代放大把一次错误转发的 load 变成 U/V 输出的 1 bit(或几 bit)差异,与 GEMM 的结果整体多 bit 破坏截然不同。两个大 SVD mask(0xE34FEFCC=21、0x7656F3B0=17)是破坏命中高流量中间体的例外。cdouble SVD 样本(0x0000001F993A6AD6,22 bit)更像 GEMM 式多 bit,不像典型 SVD 单 bit。

**v3 重测(Eigen 3.4.0 重编)**:原版 BDCSVD svd 测试重新编进 binary(系统 Eigen 3.3.8 + `-std=c++23` 有 BDCSVD `operator!=` 重写候选编译失败;把 meson 的 `eigen3_root` 指向 `/usr/local` 的 Eigen 3.4.0 修复)。5 测试 × 15min 战役:
- `eigen_svd`(float BDCSVD):**0 fail**(15min)。历史 11 fail 来自 30min stress——float SVD 每窗口率低,15min/单 seed 没中。
- `eigen_svd_cdouble_noavx512`(复数 double BDCSVD):**0 fail**(15min)。历史 1 fail,率最低。
- `eigen_svd_double2`(double BDCSVD,同家族):**1 fail on core 179**,ttf=125000ms,seed LCG:1037940591,mask=**0x0000000000000001 → 恰好 1 bit(bit 0,尾数最低位)**。破坏命中 Matrix V 的 offset [6608,0]:actual=−0x1.28673db9ef688p−8,expected=−0x1.28673db9ef689p−8——差 1 个 ULP。**这是 SVD 单 bit 特征的最强实测确认**:BDCSVD double 路径在同一缺陷下产生恰好 1 bit 翻转,与同一战役里 cdouble GEMM 的 24 bit 形成直接对比。

### 3.9 eigen_svd_cdouble_noavx512 —— 迭代 BDCSVD(复数 double)

**负载类型**:同 3.8,但 `std::complex<double>` 300×300 矩阵(M_DIM=300,"怪维度"),`BDCSVD`。用 Eigen 3.4.0 重编后可运行。

**翻转规律**。历史 `eigen_30m` 记录 1 fail(mask=0x0000001F993A6AD6,22 bit)。翻转规律:单样本不足定模式,但 22 bit 多 bit 翻转,介于 SVD 单 bit 与 GEMM 多 bit 之间。

### 3.10 eigen_sparse —— 稀疏线性代数

**负载类型**:稀疏 `Ax=b` 求解(Cholesky,实对称 A)。源码 `tests/cpu/eigen_sparse/eigen_sparse.cpp`。

**翻转规律**。历史无 fail。补测 5×15min 全 fail=0——但 `test-runtime` 仅 15.485s:稀疏求解算完一轮就 pass 退出,远未到 15min 超时。加 `--max-test-loop-count=0` 后持续运行,仍 0 fail(见正在跑的战役)。触发能力**未确定**(要么真不触发,要么窗口/足迹不足)。

### 3.11 跨负载翻转规律汇总

| 负载 | 负载类型 | store→load 模式 | 翻转 bit 数(中位/范围) | 字段偏好 | 符号位 | 规律性 |
|------|----------|----------------|----------------------|---------|--------|--------|
| movbe_dump | 整数字节交换 | 同缓冲 reload(最紧) | 17 / 10–21 | 无(整数) | n/a | 多 bit,无固定位 |
| mrn_rmw_dump | 整数ALU+转发 | 同地址 str→ldr | 35 / 1样本 | 无(整数) | n/a | 多 bit(最密) |
| memcpy1 | 纯拷贝 | load→store→load(无ALU) | 无数据 | — | — | 从未触发 |
| eigen_gemm_float_dynamic_square | FMA GEMM | 写回→校验 | 12 / 6–21 | 尾数 85% | 1/207 | 多 bit,尾数 |
| eigen_gemm_double_dynamic_square | FMA GEMM | 写回→校验 | 28 / 20–39 | 尾数 93% | 0/355 | 多 bit(最密FP) |
| eigen_gemm_double14 | GEMM+copy/verify | 额外往返 | 无数据(isApprox) | — | — | 仅触发确认(最频繁) |
| eigen_gemm_cdouble_dynamic_square | 复数FMA GEMM | 写回→校验 | 24 / 1重测样本(+历史1个28bit) | 尾数+低位指数 | — | 低率;v3重测1 fail/24bit on 179(早先战役0) |
| eigen_svd | 迭代BDCSVD | 迭代驱动 | 3 / 1–21(历史) | 尾数低位 | — | **多为单 bit**(5/11历史);v3重测0 fail(低率,15min没中) |
| eigen_svd_double2 | 迭代BDCSVD(double) | 迭代驱动 | 1 / 1重测样本 | bit 0(尾数LSB) | — | **恰好1 bit**(v3重测)——确认SVD单bit特征 |
| eigen_svd_cdouble_noavx512 | 迭代BDCSVD复数 | 迭代驱动 | 22 / 1历史样本 | — | — | 最低率;v3重测0 fail |
| eigen_sparse | 稀疏求解 | — | 无数据 | — | — | 未触发(秒退) |

**所有路径共同的规律**:(1)每次翻转都在 core 179;(2)无固定位(xor 每次 fail 都不同);(3)**多 bit 为主**(仅 SVD 出现单 bit,且那是少数样本)——排除单 bit SEU。**按负载类型分裂的规律**:紧密 store→reload(movbe/mrn_rmw)和数据密集写回(GEMM)→ 多 bit 整体破坏;迭代驱动(SVD)→ 单 bit 放大扰动;纯拷贝(memcpy1)→ 仅触发(实为从未触发)。分裂沿 **load/store 访问模式**而非算术类型,支持缺陷点在 core 179 的 load/store 通路。

---

## 四、位翻转位置统计(36 样本 / 562 bit)

### 4.1 样本基础
- 36 个有效翻转样本,562 个翻转 bit 点,全 on core 179、全核心+自动seed触发。
- float32:23 样本/207 bit;double64:13 样本/355 bit。
- 来源:历史 30min stress log(34 个框架 `data-miscompare` 块,含 actual/expected/mask=xor)+ dump 战役 2 样本。
- 注:mrn_rmw 整数 xor 不参与 IEEE754 区域分析;double14 因 isApprox 无数据。

### 4.2 发现 1——翻转集中在尾数,符号位几乎免疫

| 精度 | 符号位 | 指数 | 尾数 |
|------|--------|------|------|
| float32(207 bit) | 1(0%) | 29(14%) | **177(85%)** |
| double64(355 bit) | 0(0%) | 24(6%) | **331(93%)** |

- 符号位 562 次里翻转 0–1 次,**几乎免疫**→排除"正负量级反转"类破坏。
- 尾数占绝对多数(85%→93%);精度越高占比越大(尾数位宽 23→52,落点更多)。
- 指数被命中但是少数(6–14%)——破坏能改量级但不偏好。

### 4.3 发现 2——翻转 bit 数与计算路径强相关(非随机)

| 测试路径 | 样本 | 翻转 bit 数 | 中位 | 模式 |
|----------|------|------------|------|------|
| eigen_svd | 11 | 1,1,1,1,1,3,3,5,7,18,21 | 3 | **多为单 bit**——5/11 恰好 1 bit;像单点扰动经迭代计算放大 |
| eigen_gemm_float_dynamic_square | 11 | 6,9,9,10,12,12,12,13,13,18,21 | 12 | 中等多 bit |
| eigen_gemm_double_dynamic_square | 11 | 20,21,25,25,27,28,28,29,32,38,39 | 28 | **高密度多 bit**——像结果数据整体被破坏 |

**解释**:GEMM(数据密集 load/store)→ 多 bit 整体破坏;SVD(迭代计算)→ 多为单 bit,扰动经迭代放大。破坏点可能在**存储/load-store 通路**而非 ALU 计算核;GEMM 的多 bit 更像 cache line 级破坏。

### 4.4 发现 3——缺陷跨指令路径,非单指令 bug
同一 core 179 + 同一触发条件,movbe(字节交换)/mrn_rmw(整数ALU+store-load转发)/eigen_gemm(FMA浮点)/eigen_svd(迭代)全触发。"movbe 字节交换路径缺陷"的早期定性需扩展为:**core 179 上跨多种执行路径的多 bit 数据级破坏,多核并发下触发**。指令是*触发载体*,不是*原因*。

### 4.5 分布排除的假设
- **单 bit SEU(宇宙射线/α 粒子)**:仅 5/36 样本单 bit(全在 SVD),且全锁定一核 + 特定并发条件,时空均不随机。
- **ALU 语义/符号逻辑错**:符号位免疫 + 尾数集中 + 路径相关计数 ⇒ ALU 结果语义不是翻转点。
- **固定位**:xor 每次 fail 都不同,无恒定位。

---

## 五、memcpy1 误归更正

此前报告(含 V3 早期版本)称"memcpy1 历史 6 fail on 179"。**经 byte-offset 精确定位核实,这是解析错误**:

| 日志 | 之前归为 | 实际(精确定位) | seed |
|------|---------|---------------|------|
| all_cores_10m_20260815(89G) | memcpy1 ×4 | **movbe ×4** | LCG:163174881 |
| all_cores_10m_20260816(9.6G) | memcpy1 ×2 | **movbe ×2** | LCG:1410093522 |

错误根因:memcpy1 的源码每迭代 `fprintf(stderr,...)`,单个分片日志撑到 MB 级;此前用 awk 按 `- test:` 分块时,在 fail 附近匹配错位,把 movbe 的 fail 错记成 memcpy1。改用 byte-offset(fail 前最近的 `- test:`)即得正确归属:全是 movbe。

**memcpy1 实际从未触发 SDC**:`all_tests_2h` 里 496 个 memcpy1 分片全 pass;5min(seed 固定)+ 10min(11180 seed 轮换)专门重测均 0 fail。

---

## 六、触发条件定位(控制探针 A–H)

通过控制探针(每次只改一个变量,全自动 seed 重测,纠正了 V1 的固定 seed 方法论错误)确立:

### 6.1 必需条件(移除该条件 → PASS)

| 探针 | 移除什么 | 结果 | 结论 |
|------|---------|------|------|
| A | store(无存副作用) | PASS | **需要**一个 store |
| D | 地址推进(固定地址存) | PASS | store 地址必须**推进** |
| E | 同 LLC domain(swapped 在不同 NUMA) | PASS | store 与 reload 须**同 LLC domain** |
| F | 跨线步进(存限一个 cache line,`i&15`) | PASS | store 须**跨 cache line 推进** |

### 6.2 非判别条件

| 探针 | 改什么 | 触发率 | 结论 |
|------|--------|--------|------|
| baseline | 无 | ~100%(5/5 seed) | 参考 |
| H(LINES=512) | 加 `and x2,x19,x20`(语义 no-op,`i&16383==i`);store 地址=baseline,store↔reload 保持**back-to-back**(objdump 证实) | ~10%(1/10) | 一条 no-op ALU 指令把率从 100%→10%,**不碰**地址/足迹/back-to-back |
| X | 加 `eor w?,w?,wzr`(语义 no-op);store 足迹=baseline,但 store↔reload **不 back-to-back**(编译器在 str、ldr 间保留第2个 rev) | ~20%(1/5) | 破坏 back-to-back **不消除**触发;率与 H 同量级 |

**判别条件是**:指令调度时序相位。H(~10%)和 X(~20%)的共性是热路径多了一条语义 no-op 的 ALU 指令。加上它——不改 store 地址、足迹、(H 的)back-to-back 邻接——就把确定性触发(~100%)崩塌为概率性(~10–20%)。缺陷是 core 179 store-buffer/load-buffer 的**逐迭代流水线相位竞态**,1 个 issue 槽的相位偏移概率性错误转发/读取。

⚠ 样本限制:H 1/10、X 1/5,差异在这样本量下不显著。显著的是:两者都远低于 baseline 100%、远高于 0%。结论是方向性的(时序相位是因,back-to-back 不是),非精确率曲线。

---

## 七、结论

### 7.1 宏观定位(高置信度,锁定)
- **核**:逻辑 179(NUMA 7,module 23340),每个 cpu-mask 的 bit 179,从不漂移。
- **翻转点**:刚读输入字的 reload `ldr`(movbe 第 4 条热循环指令;mrn_rmw、GEMM 写回路径结构类比的 reload)。
- **必需条件**:有 store + store 地址推进 + store 与 reload 同 LLC domain + store 跨 cache line 推进。
- **跨路径**:经控制战役跨 6+ 工作负载类型确证——缺陷在 core 179 公共 store/load 管线,非任何指令语义。

### 7.2 微架构机制(定性锁定,未量化)
- **原因**:core 179 load/store 单元或 store-buffer 转发路径的指令调度时序相位竞态。热路径一条语义 no-op ALU 指令偏移逐迭代 issue 相位,把确定性触发(~100%)崩塌为概率性(~10–20%)。
- **翻转特征**:尾数集中(85–93%)、符号免疫、路径相关计数(SVD 单 bit ↔ GEMM 多 bit)⇒ 数据通路/cache line 级破坏,非 ALU 语义错,非单 bit SEU。
- **已排除**:store↔reload back-to-back 邻接**不是**判别条件(探针 X 破坏它仍触发);H mask **不是**靠破坏邻接杀死缺陷(objdump:mask 在循环顶,邻接保留)。

### 7.3 未量化项
精确流水线阶段/相位窗口宽度;逐指令类型敏感度曲线;触发率-额外指令函数;cdouble 路径的真实(近零?)率;eigen_sparse 的触发能力。需每探针 ≥30 seed 战役 + core 179 上 PMU/store-buffer 事件采样。

### 7.4 方法论要点
- **不能固定 LCG seed**:固定 seed = 单一输入模式,SDC 是 seed 敏感的,retry 复用同 seed 会误判。所有探针用自动 seed。
- **dump 变体原则**:热循环字节级不动,dump 全放 cold 失败分支。加任何热路径快照或 volatile 都改变指令调度,可能让缺陷不触发(H/X 探针的前车之鉴)。经验证对 mrn_rmw、eigen_gemm float/double/cdouble 同样有效(每对原始/dump fail 数一致)。
- **框架 memcmp_or_fail 已自带 data-miscompare 块**(含 actual/expected/mask=xor),所以位翻转位置统计**不一定需要 dump 变体**,历史日志足够。dump 变体只在需区分 input/golden(同缓冲不同时刻读)时才需要。
- **大日志解析用 byte-offset**:memcpy1 的 fprintf 会撑爆分片日志,awk 按 `- test:` 分块易错位;用 fail 前最近 `- test:` 的 byte-offset 定位才准。

---

## 八、日志归档

所有战役日志在 `movbe_log/`,按战役子目录 + `README.md` 索引。关键目录:
- `movbe_log/baseline/` —— movbe_dump 5x + repro(15 个 xor 样本)
- `movbe_log/dump_variants_15min/` —— 8×15min dump 战役(mrn_rmw + eigen_gemm 原始/dump)
- `movbe_log/eigen_complement/` —— double14 + sparse 补采样
- `movbe_log/other_sdc/eigen_core179_30m_stress_logs/` —— 30min stress(44 fail,eigen 主力)
- `movbe_log/other_sdc/all_tests_2h_*.log`、`all_cores_10m_*.log` —— 全套并发历史日志
- `movbe_log/eigen_svd_cdouble_5tests/` —— Eigen 3.4.0 重编后的 svd/sparse/cdouble 战役
- `movbe_log/memcpy1_mrn_5min/`、`memcpy1_10min_nofracture/` —— memcpy1 不触发验证

报告本身:
- `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT.md`(V1,2026-08-18,已被取代,留作历史)
- `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT_V2.md`(V2,2026-08-19)
- `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT_V3.md`(V3,2026-08-20,英文,最新)
- `docs/CORE179_SDC_REPORT_CN.md`(本报告,中文,2026-08-20)
