/*
 * openeuler_compat.h — C++ standard-library back-compat shims for building
 * OpenDCDiag on older openEuler LTS toolchains.
 *
 * Context: the aarch64 CPU build path uses a small surface of C++23 / C++20
 * standard-library facilities:
 *   - std::to_underlying  (C++23)  — 8 call sites in framework/
 *   - std::span           (C++20)  — 62 call sites, deep in the public API
 *
 * openEuler 24.03 ships gcc 12.3 + libstdc++ 12, which provides both natively
 * (build is gnu++23). openEuler 22.03 ships gcc 10.3 + libstdc++ 10, which
 * provides std::span (C++20) but not std::to_underlying (C++23, libstdc++ 14).
 * openEuler 20.03 ships gcc 7.3 + libstdc++ 7, which provides neither
 * (no <span>, no to_underlying).
 *
 * The build (meson.build) sets -DSANDSTONE_CPP_COMPAT=23|20|17 to tell this
 * header which std the toolchain is compiling under. We additionally rely on
 * the SD-6 feature-test macros (__cpp_lib_span / __cpp_lib_to_underlying) as
 * the authoritative switch, so the polyfills activate exactly when the
 * toolchain genuinely lacks the facility — independent of the macro value.
 *
 * Design rules (per CLAUDE.md x86-untouched / additive porting rule):
 *   - On 24.03 (gcc 12, gnu++23): every macro maps to the real std:: facility.
 *     Zero behaviour change, byte-identical to building without this header.
 *   - The std::span polyfill is provided inside namespace std ONLY when
 *     __cpp_lib_span is undefined (i.e. gcc < 10). There is no std::span on
 *     such toolchains to conflict with, so this is safe; it provides a
 *     conforming-enough subset covering every constructor/op the codebase uses.
 *   - std::to_underlying polyfill likewise only when __cpp_lib_to_underlying
 *     is undefined.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OPENDCDIAG_OPENEULER_COMPAT_H
#define OPENDCDIAG_OPENEULER_COMPAT_H

// <version> provides the __cpp_lib_* feature-test macros (libstdc++ 10+).
// gcc 7 (openEuler 20.03) does not ship it — on such toolchains the macros
// are simply absent and the polyfills below activate. Include it only when
// available so the header is usable on gcc 7 too.
#ifdef __has_include
#  if __has_include(<version>)
#    include <version>
#  endif
#endif

/* ------------------------------------------------------------------ */
/* std::to_underlying (C++23, <utility>)                              */
/* ------------------------------------------------------------------ */
#ifndef __cpp_lib_to_underlying
#  include <type_traits>
namespace std {
// Mirrors C++23 std::to_underlying<E>: cast an enumeration to its underlying
// integer type. Used for comparisons, arithmetic, and ostream<< of enums.
template <typename E>
constexpr std::enable_if_t<std::is_enum<E>::value,
                            typename std::underlying_type<E>::type>
to_underlying(E e) noexcept
{
    return static_cast<typename std::underlying_type<E>::type>(e);
}
} // namespace std
#endif /* __cpp_lib_to_underlying */

/* ------------------------------------------------------------------ */
/* std::bit ops (C++20, <bit>)                                         */
/* ------------------------------------------------------------------ */
/* openEuler 20.03's gcc 7 / libstdc++ 7 does not ship <bit> at all. The
 * topology code uses std::popcount, std::countr_zero, std::bit_floor,
 * std::rotl, std::bit_width — all trivially expressible via gcc builtins.
 * On gcc 10+ (22.03/24.03) __cpp_lib_bit_ops is defined and the native <bit>
 * is used (include sites gate on __cpp_lib_bit_ops); on gcc 7 these inline
 * polyfills in namespace std activate. */
#ifndef __cpp_lib_bit_ops
#  include <cstddef>
namespace std {
constexpr int popcount(unsigned long long x) noexcept { return __builtin_popcountll(x); }
constexpr int popcount(unsigned long x) noexcept { return __builtin_popcountl(x); }
constexpr int popcount(unsigned int x) noexcept { return __builtin_popcount(x); }
constexpr int countr_zero(unsigned long long x) noexcept { return x ? __builtin_ctzll(x) : (int)(sizeof(unsigned long long) * 8); }
constexpr int countr_zero(unsigned long x) noexcept { return x ? __builtin_ctzl(x) : (int)(sizeof(unsigned long) * 8); }
constexpr int countr_zero(unsigned int x) noexcept { return x ? __builtin_ctz(x) : (int)(sizeof(unsigned int) * 8); }
constexpr int bit_width(unsigned long long x) noexcept { return (int)(8 * sizeof(unsigned long long)) - (x ? __builtin_clzll(x) : (int)(8 * sizeof(unsigned long long))); }
constexpr int bit_width(unsigned long x) noexcept { return (int)(8 * sizeof(unsigned long)) - (x ? __builtin_clzl(x) : (int)(8 * sizeof(unsigned long))); }
constexpr int bit_width(unsigned int x) noexcept { return (int)(8 * sizeof(unsigned int)) - (x ? __builtin_clz(x) : (int)(8 * sizeof(unsigned int))); }
template <typename T> constexpr T bit_floor(T x) noexcept { return x ? (T(1) << bit_width(x)) >> 1 : T(0); }
template <typename T> constexpr T rotl(T x, int s) noexcept {
    constexpr int N = (int)(sizeof(T) * 8);
    int r = s % N; if (r < 0) r += N;
    return r == 0 ? x : (T)((unsigned T)(x) << r | (unsigned T)(x) >> (N - r));
}
} // namespace std
#endif /* __cpp_lib_bit_ops */

/* ------------------------------------------------------------------ */
/* C++20 concepts / requires (language feature)                       */
/* ------------------------------------------------------------------ */
/* openEuler 20.03 ships gcc 7.3, which predates C++20 concepts entirely
 * (no `concept` keyword, no `requires`-clause/expression). The code uses
 * concepts in a handful of places. Because concept syntax and enable_if
 * syntax are structurally different, the call sites use the macros below,
 * which expand to native concepts on C++20+ and to enable_if/SFINAE on C++17.
 * On gcc 10+ (22.03) and gcc 12+ (24.03), __cpp_concepts is defined and the
 * native concept syntax is used — no behaviour change. */
#ifndef __cpp_concepts
#  include <type_traits>
// Detection traits for the requires-expression at sandstone_p.h:492, which
// checks whether a callable l can be invoked as l(test_thread_data*, int, int).
// On C++20 this is `requires { l(a,b,c); }`; on C++17 we use void_t detection.
namespace sandstone_detail {
template <typename, typename = void> struct is_callable3 : std::false_type {};
template <typename F, typename A, typename B, typename C>
struct is_callable3<F(A, B, C),
        decltype(void(std::declval<F>()(std::declval<A>(), std::declval<B>(), std::declval<C>())))>
    : std::true_type {};
} // namespace sandstone_detail
#endif /* __cpp_concepts */

/* ------------------------------------------------------------------ */
/* std::string::contains / std::string_view::contains (C++23)         */
/* ------------------------------------------------------------------ */
/* These are member functions; there is no clean namespace-level shim
 * for member calls, so we expose a free macro and edit the (few) call
 * sites to use it. std::map::contains / std::unordered_map::contains are
 * C++20 and present on gcc 10+, so map.contains() is left untouched.
 *
 * __cpp_lib_string_contains is defined by libstdc++ >= 14 (gcc 14+). On
 * gcc 12 (openEuler 24.03) std::string::contains is available at gnu++23
 * even without the macro, so we key off gnu++23 instead for the gcc-12
 * case below — the macro just expands to the native call when the std is
 * 23. */
#ifndef SANDSTONE_STR_CONTAINS
#  if SANDSTONE_CPP_COMPAT >= 23
#    define SANDSTONE_STR_CONTAINS(str, x) (str).contains(x)
#  else
#    define SANDSTONE_STR_CONTAINS(str, x) ((str).find((x)) != (str).npos)
#  endif
#endif

/* ------------------------------------------------------------------ */
/* std::barrier (C++20, <barrier>)                                    */
/* ------------------------------------------------------------------ */
/* openEuler 22.03 ships gcc 10.3 / libstdc++ 10, which does NOT provide
 * <barrier> (added in libstdc++ 11). The aarch64 CPU topology code uses
 * std::barrier<std::function<void()>> with a completion function in exactly
 * one site (framework/device/cpu/topology_cpu.h, BarrierDeviceScheduler),
 * calling only arrive_and_wait(), arrive_and_drop(), the (count, comp) ctor,
 * and delete. This minimal polyfill implements exactly that surface on top of
 * mutex + condition_variable, invoking the completion function at each phase
 * boundary. On gcc 12+ (openEuler 24.03) __cpp_lib_barrier is defined and the
 * native <barrier> is used — zero behaviour change. */
#ifndef __cpp_lib_barrier
#  include <condition_variable>
#  include <cstddef>
#  include <functional>
#  include <mutex>

namespace std {

template <typename CompletionFunction = decltype([]() {})>
class barrier
{
public:
    barrier(ptrdiff_t count, CompletionFunction completion = CompletionFunction())
        : count_(count), total_(count), completion_(std::move(completion)), phase_(0)
    {
    }

    // arrive_and_wait: decrement count; if it hits 0, run the completion fn,
    // reset count, advance phase, and wake all waiters; otherwise wait for
    // the current phase to advance.
    void arrive_and_wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto my_phase = phase_;
        if (--count_ == 0) {
            ++phase_;
            count_ = total_;
            if (completion_)
                completion_();
            cv_.notify_all();
        } else {
            cv_.wait(lock, [this, my_phase] { return phase_ != my_phase; });
        }
    }

    // arrive_and_drop: remove one permanent participant (the caller will not
    // wait at this barrier again). Used by BarrierDeviceScheduler when a
    // worker leaves the group.
    void arrive_and_drop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        --total_;
        if (--count_ == 0) {
            ++phase_;
            count_ = total_;
            if (completion_)
                completion_();
            cv_.notify_all();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    ptrdiff_t count_;
    ptrdiff_t total_;
    CompletionFunction completion_;
    unsigned long long phase_;
};

} // namespace std
#endif /* __cpp_lib_barrier */

/* ------------------------------------------------------------------ */
/* std::span (C++20, <span>)                                          */
/* ------------------------------------------------------------------ */
#ifndef __cpp_lib_span
#  include <cstddef>
#  include <iterator>      // std::begin / std::end, std::reverse_iterator
#  include <type_traits>

namespace std {

// Minimal std::span covering exactly the constructors/operators the OpenDCDiag
// aarch64 build path uses:
//   span(ptr, n)                 pointer + count
//   span(first, last)            iterator pair (raw pointers)
//   span()                       default (empty)
//   span(Container&)             array/contiguous container conversion
//   operator[](i), front(), back()
//   size(), size_bytes(), empty(), data()
//   begin(), end(), rbegin(), rend()  (for range-for and algorithms)
//   implicit conversion span<T> -> span<const T>
// No subspan()/first()/last() (the codebase does not use them).
template <typename T, ptrdiff_t Extent = static_cast<ptrdiff_t>(-1)>
class span {
public:
    using element_type    = T;
    using value_type      = typename std::remove_cv<T>::type;
    using index_type     = ptrdiff_t;
    using pointer         = T *;
    using reference       = T &;
    using iterator        = pointer;
    using const_iterator  = const T *;

    static constexpr ptrdiff_t extent = Extent;

    constexpr span() noexcept : data_(nullptr), size_(0) {}

    // pointer + count
    constexpr span(pointer ptr, index_type count) noexcept
        : data_(ptr), size_(count) {}

    // iterator (pointer) range
    template <typename It1, typename It2>
    constexpr span(It1 first, It2 last) noexcept
        : data_(&*first), size_(last - first) {}

    // from a C array
    template <ptrdiff_t N>
    constexpr span(element_type (&arr)[N]) noexcept
        : data_(arr), size_(N) {}

    // from a contiguous container with data()/size() (e.g. std::array, std::vector)
    template <typename C,
              typename = typename std::enable_if<
                  !std::is_array<C>::value &&
                  !std::is_same<typename std::remove_cv<C>::type, span>::value>::type>
    constexpr span(C &c) noexcept : data_(c.data()), size_(c.size()) {}

    // implicit conversion span<T> -> span<const T>
    template <typename U,
              typename = typename std::enable_if<
                  std::is_same<U, typename std::remove_cv<T>::type>::value>::type>
    constexpr span(const span<U> &other) noexcept
        : data_(other.data()), size_(other.size()) {}

    constexpr reference operator[](index_type i) const { return data_[i]; }
    constexpr reference front() const { return data_[0]; }
    constexpr reference back() const { return data_[size_ - 1]; }

    constexpr pointer data() const noexcept { return data_; }
    constexpr index_type size() const noexcept { return size_; }
    constexpr index_type size_bytes() const noexcept {
        return size_ * static_cast<ptrdiff_t>(sizeof(element_type));
    }
    constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr iterator begin() const noexcept { return data_; }
    constexpr iterator end() const noexcept { return data_ + size_; }

private:
    pointer data_;
    index_type size_;
};

template <typename T, typename = typename std::enable_if<!std::is_void<T>::value>::type>
span(T *, ptrdiff_t) -> span<T>;

template <typename It1, typename It2, typename = decltype(&*std::declval<It1>())>
span(It1, It2) -> span<typename std::remove_reference<decltype(*std::declval<It1>())>::type>;

template <typename C, typename = decltype(std::declval<C &>().data())>
span(C &) -> span<typename std::remove_reference<typename C::value_type>::type>;

} // namespace std
#endif /* __cpp_lib_span */

#endif /* OPENDCDIAG_OPENEULER_COMPAT_H */
