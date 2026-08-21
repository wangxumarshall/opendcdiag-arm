#ifndef NEON_VECTORS_H
#define NEON_VECTORS_H

#include <arm_neon.h>

#include <cstddef>
#include <cstdint>

namespace {

constexpr int SIMD_WIDTH = 128;

template<int Width>
struct SimdVector;

template<>
struct SimdVector<128> {
    using type = int32x4_t;
};

template<>
struct SimdVector<256> {
    struct type {
        int32x4_t lo;
        int32x4_t hi;
    };
};

template<>
struct SimdVector<512> {
    struct type {
        int32x4_t v0;
        int32x4_t v1;
        int32x4_t v2;
        int32x4_t v3;
    };
};

template<int Width>
using SimdVectorT = typename SimdVector<Width>::type;

template<int Width>
inline SimdVectorT<Width> simd_load(const int32_t* ptr) {
    static_assert(Width == 128 || Width == 256 || Width == 512, "Unsupported width");
    if constexpr (Width == 128) {
        return vld1q_s32(ptr);
    } else if constexpr (Width == 256) {
        return {vld1q_s32(ptr), vld1q_s32(ptr + 4)};
    } else {
        return {vld1q_s32(ptr),
                vld1q_s32(ptr + 4),
                vld1q_s32(ptr + 8),
                vld1q_s32(ptr + 12)};
    }
}

template<int Width>
inline void simd_store(int32_t* ptr, SimdVectorT<Width> vec) {
    static_assert(Width == 128 || Width == 256 || Width == 512, "Unsupported width");
    if constexpr (Width == 128) {
        vst1q_s32(ptr, vec);
    } else if constexpr (Width == 256) {
        vst1q_s32(ptr, vec.lo);
        vst1q_s32(ptr + 4, vec.hi);
    } else {
        vst1q_s32(ptr, vec.v0);
        vst1q_s32(ptr + 4, vec.v1);
        vst1q_s32(ptr + 8, vec.v2);
        vst1q_s32(ptr + 12, vec.v3);
    }
}

template<int Width>
inline SimdVectorT<Width> simd_add(SimdVectorT<Width> a, SimdVectorT<Width> b) {
    static_assert(Width == 128 || Width == 256 || Width == 512, "Unsupported width");
    if constexpr (Width == 128) {
        return vaddq_s32(a, b);
    } else if constexpr (Width == 256) {
        return {vaddq_s32(a.lo, b.lo), vaddq_s32(a.hi, b.hi)};
    } else {
        return {vaddq_s32(a.v0, b.v0),
                vaddq_s32(a.v1, b.v1),
                vaddq_s32(a.v2, b.v2),
                vaddq_s32(a.v3, b.v3)};
    }
}

template<int Width>
inline SimdVectorT<Width> simd_sub(SimdVectorT<Width> a, SimdVectorT<Width> b) {
    static_assert(Width == 128 || Width == 256 || Width == 512, "Unsupported width");
    if constexpr (Width == 128) {
        return vsubq_s32(a, b);
    } else if constexpr (Width == 256) {
        return {vsubq_s32(a.lo, b.lo), vsubq_s32(a.hi, b.hi)};
    } else {
        return {vsubq_s32(a.v0, b.v0),
                vsubq_s32(a.v1, b.v1),
                vsubq_s32(a.v2, b.v2),
                vsubq_s32(a.v3, b.v3)};
    }
}

template<int Width>
inline SimdVectorT<Width> simd_mul(SimdVectorT<Width> a, SimdVectorT<Width> b) {
    static_assert(Width == 128 || Width == 256 || Width == 512, "Unsupported width");
    if constexpr (Width == 128) {
        return vmulq_s32(a, b);
    } else if constexpr (Width == 256) {
        return {vmulq_s32(a.lo, b.lo), vmulq_s32(a.hi, b.hi)};
    } else {
        return {vmulq_s32(a.v0, b.v0),
                vmulq_s32(a.v1, b.v1),
                vmulq_s32(a.v2, b.v2),
                vmulq_s32(a.v3, b.v3)};
    }
}

template<int Width>
inline SimdVectorT<Width> simd_set1(int32_t val) {
    static_assert(Width == 128 || Width == 256 || Width == 512, "Unsupported width");
    if constexpr (Width == 128) {
        return vdupq_n_s32(val);
    } else if constexpr (Width == 256) {
        auto v = vdupq_n_s32(val);
        return {v, v};
    } else {
        auto v = vdupq_n_s32(val);
        return {v, v, v, v};
    }
}

}

#endif
