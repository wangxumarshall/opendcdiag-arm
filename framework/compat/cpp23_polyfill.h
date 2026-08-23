// SPDX-License-Identifier: Apache-2.0
//
// cpp23_polyfill.h — C++23/C++20 标准库缺失符号的 polyfill,仅用于在 GCC<12 的旧
// openEuler(22.03 / 20.03,GCC 10.x)上构建 OpenDCDiag 的 ARM64 CPU 路径。
//
// 设计要点(满足 CLAUDE.md "宏隔离 / 24.03 源码字节一致" 约束):
//   * 本文件是新增文件,不改任何既有头/源码。
//   * 通过 CXXFLAGS 注入 `-DOPENEULER_22_03 (或 OPENEULER_20_03)
//     -include .../cpp23_polyfill.h` 在编译期前置包含;既有源码行无需任何 #include 改动。
//   * 所有补丁受 `__cpp_lib_*` 特性宏守卫:只在编译器/库确实缺失该符号时才生效。
//     → 在 GCC 12+(24.03,gnu++23 下这些宏全部定义)上,下面的 #if 分支全部不进入,
//       预处理器输出与未 -include 本文件时逐字节一致 → 24.03 源码字节一致得以保证。
//   * 守卫还加了 OPENEULER_22_03 / OPENEULER_20_03 双保险:即使误在 24.03 构建里
//     -include 了本文件,只要没定义这两个宏,polyfill 体仍不生效。
//
// 宏命名说明:
//   目标要求适配宏名为 "OPENEULER_22.03" / "OPENEULER_20.03",但 C 预处理器的
//   宏标识符不允许含点号('.'),#define OPENEULER_22.03 非法(点号被丢弃,实际定义成
//   OPENEULER_22)。故采用合法等价名 OPENEULER_22_03 / OPENEULER_20_03(点号→下划线,
//   工程惯例)。语义与原意图一致:仅用于隔离 22.03/20.03 的适配代码,不触碰 24.03。
//
// 覆盖范围(经实测扫描 ARM64 CPU 构建路径):
//   - std::to_underlying  (C++23, <utility>, GCC>=12)  ← 22.03/20.03 GCC10 缺
//   - std::bit_floor/ceil (C++20, <bit>,    GCC>=12 完整) ← GCC10 <bit> 不完整
//   注: std::span(GCC10 已支持)、operator<=>(GCC10 已支持)无需 polyfill。
//   注: std::format 仅用于 x86_64 路径(#ifdef __x86_64__),ARM64 不编,无需 polyfill。
#pragma once

// 只在确实命中旧编译器 + 适配宏时才动 std 命名空间。
#if (defined(OPENEULER_22_03) || defined(OPENEULER_20_03)) && defined(__cplusplus)

#include <version>      // __cpp_lib_* 特性宏( libstdc++ 10 起提供)
#include <type_traits>   // underlying_type_t
#include <cstddef>       // size_t
#include <climits>       // CHAR_BIT

// ---------------------------------------------------------------------------
// std::to_underlying  (C++23, P1682, __cpp_lib_to_underlying)
// GCC 10 ~ 11 缺;GCC 12+ 有。ARM64 CPU 路径 5 个文件用(LogLevelVerbosity 等)。
// ---------------------------------------------------------------------------
#if !defined(__cpp_lib_to_underlying)
namespace std {
    template <typename _Enum>
    constexpr typename underlying_type<_Enum>::type
    to_underlying(_Enum __e) noexcept {
        return static_cast<typename underlying_type<_Enum>::type>(__e);
    }
} // namespace std
#endif // !__cpp_lib_to_underlying

// ---------------------------------------------------------------------------
// std::bit_floor / std::bit_ceil  (C++20, <bit>, __cpp_lib_bit_ops)
// GCC 10 的 <bit> 部分缺(bit_floor/ceil 在某些 patch 级缺失或不可靠);
// 用 __builtin 等价实现,语义与标准一致(已编译期整数幂运算)。
// 仅在库确实未提供时补;GCC12+ 的 <bit> 完整,本块不生效。
// ---------------------------------------------------------------------------
#if !defined(__cpp_lib_bit_ops)
namespace std {
    template <typename _Tp>
    constexpr _Tp bit_floor(_Tp __x) noexcept {
        // __builtin_clz 对无符号;标准要求 x>0 行为,0 返回 0(与 libstdc++ 一致)。
        return __x == 0 ? _Tp(0)
                        : (_Tp(1) << (sizeof(_Tp) * CHAR_BIT - 1 - __builtin_clz(__x)));
    }
    template <typename _Tp>
    constexpr _Tp bit_ceil(_Tp __x) noexcept {
        // 最小 2^k >= x;若 x 已是 2 的幂则返回 x。
        if (__x <= 1) return _Tp(1);
        return _Tp(1) << ((sizeof(_Tp) * CHAR_BIT - 1 - __builtin_clz(__x - 1)) + 1);
    }
} // namespace std
#endif // !__cpp_lib_bit_ops

#endif // (OPENEULER_22_03 || OPENEULER_20_03) && __cplusplus

